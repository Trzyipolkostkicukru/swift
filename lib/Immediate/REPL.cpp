//===--- REPL.cpp - the integrated REPL -----------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/Immediate/Immediate.h"
#include "ImmediateImpl.h"

#include "swift/Config.h"
#include "swift/Subsystems.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/DiagnosticsFrontend.h"
#include "swift/AST/IRGenOptions.h"
#include "swift/AST/IRGenRequests.h"
#include "swift/AST/Module.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/NameLookupRequests.h"
#include "swift/Frontend/Frontend.h"
#include "swift/IDE/REPLCodeCompletion.h"
#include "swift/IDE/Utils.h"
#include "swift/SIL/SILModule.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "llvm/ExecutionEngine/MCJIT.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/DiagnosticPrinter.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/Process.h"

#if HAVE_LIBEDIT
#include <histedit.h>
#include <wchar.h>
#endif

using namespace swift;
using namespace swift::immediate;

namespace {

enum class REPLInputKind : int {
  /// The REPL got a "quit" signal.
  REPLQuit,
  /// Empty whitespace-only input.
  Empty,
  /// A REPL directive, such as ':help'.
  REPLDirective,
  /// Swift source code.
  SourceCode,
};

template<size_t N>
class ConvertForWcharSize;

template<>
class ConvertForWcharSize<2> {
public:
  static llvm::ConversionResult ConvertFromUTF8(const char** sourceStart,
                                                const char* sourceEnd,
                                                wchar_t** targetStart,
                                                wchar_t* targetEnd,
                                                llvm::ConversionFlags flags) {
    return ConvertUTF8toUTF16(reinterpret_cast<const llvm::UTF8**>(sourceStart),
                              reinterpret_cast<const llvm::UTF8*>(sourceEnd),
                              reinterpret_cast<llvm::UTF16**>(targetStart),
                              reinterpret_cast<llvm::UTF16*>(targetEnd),
                              flags);
  }
  
  static llvm::ConversionResult ConvertToUTF8(const wchar_t** sourceStart,
                                              const wchar_t* sourceEnd,
                                              char** targetStart,
                                              char* targetEnd,
                                              llvm::ConversionFlags flags) {
    return ConvertUTF16toUTF8(
                             reinterpret_cast<const llvm::UTF16**>(sourceStart),
                             reinterpret_cast<const llvm::UTF16*>(sourceEnd),
                             reinterpret_cast<llvm::UTF8**>(targetStart),
                             reinterpret_cast<llvm::UTF8*>(targetEnd),
                             flags);
  }
};

template<>
class ConvertForWcharSize<4> {
public:
  static llvm::ConversionResult ConvertFromUTF8(const char** sourceStart,
                                                const char* sourceEnd,
                                                wchar_t** targetStart,
                                                wchar_t* targetEnd,
                                                llvm::ConversionFlags flags) {
    return ConvertUTF8toUTF32(reinterpret_cast<const llvm::UTF8**>(sourceStart),
                              reinterpret_cast<const llvm::UTF8*>(sourceEnd),
                              reinterpret_cast<llvm::UTF32**>(targetStart),
                              reinterpret_cast<llvm::UTF32*>(targetEnd),
                              flags);
  }
  
  static llvm::ConversionResult ConvertToUTF8(const wchar_t** sourceStart,
                                              const wchar_t* sourceEnd,
                                              char** targetStart,
                                              char* targetEnd,
                                              llvm::ConversionFlags flags) {
    return ConvertUTF32toUTF8(
                             reinterpret_cast<const llvm::UTF32**>(sourceStart),
                             reinterpret_cast<const llvm::UTF32*>(sourceEnd),
                             reinterpret_cast<llvm::UTF8**>(targetStart),
                             reinterpret_cast<llvm::UTF8*>(targetEnd),
                             flags);
  }
};

using Convert = ConvertForWcharSize<sizeof(wchar_t)>;

#if HAVE_LIBEDIT
static void convertFromUTF8(llvm::StringRef utf8,
                            llvm::SmallVectorImpl<wchar_t> &out) {
  size_t reserve = out.size() + utf8.size();
  out.reserve(reserve);
  const char *utf8_begin = utf8.begin();
  wchar_t *wide_begin = out.end();
  auto res = Convert::ConvertFromUTF8(&utf8_begin, utf8.end(),
                                      &wide_begin, out.data() + reserve,
                                      llvm::lenientConversion);
  assert(res == llvm::conversionOK && "utf8-to-wide conversion failed!");
  (void)res;
  out.set_size(wide_begin - out.begin());
}

static void convertToUTF8(llvm::ArrayRef<wchar_t> wide,
                          llvm::SmallVectorImpl<char> &out) {
  size_t reserve = out.size() + wide.size()*4;
  out.reserve(reserve);
  const wchar_t *wide_begin = wide.begin();
  char *utf8_begin = out.end();
  auto res = Convert::ConvertToUTF8(&wide_begin, wide.end(),
                                    &utf8_begin, out.data() + reserve,
                                    llvm::lenientConversion);
  assert(res == llvm::conversionOK && "wide-to-utf8 conversion failed!");
  (void)res;
  out.set_size(utf8_begin - out.begin());
}
#endif

} // end anonymous namespace

#if HAVE_LIBEDIT

static ModuleDecl *
typeCheckREPLInput(ModuleDecl *MostRecentModule, StringRef Name,
                   std::unique_ptr<llvm::MemoryBuffer> Buffer) {
  assert(MostRecentModule);
  ASTContext &Ctx = MostRecentModule->getASTContext();

  // Import the last module.
  ImplicitImportInfo implicitImports;
  implicitImports.AdditionalModules.emplace_back(MostRecentModule,
                                                 /*exported*/ false);

  // Carry over the private imports from the last module.
  SmallVector<ModuleDecl::ImportedModule, 8> imports;
  MostRecentModule->getImportedModules(imports,
                                       ModuleDecl::ImportFilterKind::Private);
  for (auto &import : imports) {
    implicitImports.AdditionalModules.emplace_back(import.importedModule,
                                                   /*exported*/ true);
  }

  auto *REPLModule =
      ModuleDecl::create(Ctx.getIdentifier(Name), Ctx, implicitImports);
  auto BufferID = Ctx.SourceMgr.addNewSourceBuffer(std::move(Buffer));
  auto &REPLInputFile =
      *new (Ctx) SourceFile(*REPLModule, SourceFileKind::REPL, BufferID);
  REPLModule->addFile(REPLInputFile);
  performTypeChecking(REPLInputFile);
  return REPLModule;
}

/// An arbitrary, otherwise-unused char value that editline interprets as
/// entering/leaving "literal mode", meaning it passes prompt characters through
/// to the terminal without affecting the line state. This prevents color
/// escape sequences from interfering with editline's internal state.
static constexpr wchar_t LITERAL_MODE_CHAR = L'\1';

/// Append a terminal escape sequence in "literal mode" so that editline
/// ignores it.
static void appendEscapeSequence(SmallVectorImpl<wchar_t> &dest,
                                 llvm::StringRef src)
{
  dest.push_back(LITERAL_MODE_CHAR);
  convertFromUTF8(src, dest);
  dest.push_back(LITERAL_MODE_CHAR);
}

/// The main REPL prompt string.
static const wchar_t * const PS1 = L"(swift) ";
/// The REPL prompt string for line continuations.
static const wchar_t * const PS2 = L"        ";

class REPLInput;
class REPLEnvironment;

namespace {
/// Observe that we are processing REPL input. Dump source and reset any
/// colorization before dying.
class PrettyStackTraceREPL : public llvm::PrettyStackTraceEntry {
  REPLInput &Input;
public:
  PrettyStackTraceREPL(REPLInput &Input) : Input(Input) {}
  
  void print(llvm::raw_ostream &out) const override;
};
} // end anonymous namespace

namespace {
static void linkerDiagnosticHandlerNoCtx(const llvm::DiagnosticInfo &DI) {
  if (DI.getSeverity() != llvm::DS_Error)
    return;

  std::string MsgStorage;
  {
    llvm::raw_string_ostream Stream(MsgStorage);
    llvm::DiagnosticPrinterRawOStream DP(Stream);
    DI.print(DP);
  }
  llvm::errs() << "Error linking swift modules\n";
  llvm::errs() << MsgStorage << "\n";
}



static void linkerDiagnosticHandler(const llvm::DiagnosticInfo &DI,
                                    void *Context) {
  // This assert self documents our precondition that Context is always
  // nullptr. It seems that parts of LLVM are using the flexibility of having a
  // context. We don't really care about this.
  assert(Context == nullptr && "We assume Context is always a nullptr");

  return linkerDiagnosticHandlerNoCtx(DI);
}
} // end anonymous namespace

/// EditLine wrapper that implements the user interface behavior for reading
/// user input to the REPL. All of its methods must be usable from a separate
/// thread and so shouldn't touch anything outside of the EditLine, History,
/// and member object state.
///
/// FIXME: Need the module for completions! Currently REPLRunLoop uses
/// synchronous messaging between the REPLInput thread and the main thread,
/// and client code shouldn't have access to the AST, so only one thread will
/// be accessing the module at a time. However, if REPLRunLoop
/// (or a new REPL application) ever requires asynchronous messaging between
/// REPLInput and REPLEnvironment, or if client code expected to be able to
/// grovel into the REPL's AST, then locking will be necessary to serialize
/// access to the AST.
class REPLInput {
  PrettyStackTraceREPL StackTrace;  
  
  EditLine *e;
  HistoryW *h;
  size_t PromptContinuationLevel;
  bool NeedPromptContinuation;
  bool ShowColors;
  bool PromptedForLine;
  bool Outdented;
  REPLCompletions completions;
  
  llvm::SmallVector<wchar_t, 80> PromptString;

  /// A buffer for all lines that the user entered, but we have not parsed yet.
  llvm::SmallString<128> CurrentLines;

  llvm::SmallString<16> CodeCompletionErasedBytes;

public:
  REPLEnvironment &Env;
  bool Autoindent;

  REPLInput(REPLEnvironment &env)
    : StackTrace(*this), Env(env), Autoindent(true)
  {
    // Only show colors if both stderr and stdout have colors.
    ShowColors = llvm::errs().has_colors() && llvm::outs().has_colors();
    
    // Make sure the terminal color gets restored when the REPL is quit.
    if (ShowColors)
      atexit([] {
        llvm::outs().resetColor();
        llvm::errs().resetColor();
      });

    e = el_init("swift", stdin, stdout, stderr);
    h = history_winit();
    PromptContinuationLevel = 0;
    el_wset(e, EL_EDITOR, L"emacs");
    el_wset(e, EL_PROMPT_ESC, PromptFn, LITERAL_MODE_CHAR);
    el_wset(e, EL_CLIENTDATA, (void*)this);
    el_wset(e, EL_HIST, history, h);
    el_wset(e, EL_SIGNAL, 1);
    el_wset(e, EL_GETCFN, GetCharFn);
    
    // Provide special outdenting behavior for '}' and ':'.
    el_wset(e, EL_ADDFN, L"swift-close-brace", L"Reduce {} indentation level",
            BindingFn<&REPLInput::onCloseBrace>);
    el_wset(e, EL_BIND, L"}", L"swift-close-brace", nullptr);
    
    el_wset(e, EL_ADDFN, L"swift-colon", L"Reduce label indentation level",
            BindingFn<&REPLInput::onColon>);
    el_wset(e, EL_BIND, L":", L"swift-colon", nullptr);
    
    // Provide special indent/completion behavior for tab.
    el_wset(e, EL_ADDFN, L"swift-indent-or-complete",
           L"Indent line or trigger completion",
           BindingFn<&REPLInput::onIndentOrComplete>);
    el_wset(e, EL_BIND, L"\t", L"swift-indent-or-complete", nullptr);

    el_wset(e, EL_ADDFN, L"swift-complete",
            L"Trigger completion",
            BindingFn<&REPLInput::onComplete>);
    
    // Provide some common bindings to complement editline's defaults.
    // ^W should delete previous word, not the entire line.
    el_wset(e, EL_BIND, L"\x17", L"ed-delete-prev-word", nullptr);
    // ^_ should undo.
    el_wset(e, EL_BIND, L"\x1f", L"vi-undo", nullptr);
    
    HistEventW ev;
    history_w(h, &ev, H_SETSIZE, 800);
  }
  
  ~REPLInput() {
    if (ShowColors)
      llvm::outs().resetColor();

    // FIXME: This should not be needed, but seems to help when stdout is being
    // redirected to a file.  Perhaps there is some underlying editline bug
    // where it is setting stdout into some weird state and not restoring it
    // with el_end?
    llvm::outs().flush();
    fflush(stdout);
    el_end(e);
  }

  REPLInputKind getREPLInput(SmallVectorImpl<char> &Result) {
    ide::SourceCompleteResult SCR;
    SCR.IsComplete = true;
    unsigned CurChunkLines = 0;
    wchar_t TotalLine[4096] = L"";

    CurrentLines.clear();

    // Reset color before showing the prompt.
    if (ShowColors)
      llvm::outs().resetColor();
    
    do {
      // Read one line.
      PromptContinuationLevel = SCR.IndentLevel;
      NeedPromptContinuation = !SCR.IsComplete;
      PromptedForLine = false;
      Outdented = false;
      int LineCount;
      size_t LineStart = CurrentLines.size();
      const wchar_t* WLine = el_wgets(e, &LineCount);
      if (!WLine) {
        // End-of-file.
        if (PromptedForLine)
          llvm::outs() << "\n";
        return REPLInputKind::REPLQuit;
      }
      
      if (Autoindent) {
        size_t indent = PromptContinuationLevel*2;
        CurrentLines.append(indent, ' ');
      }
      
      size_t WLineLength = wcslen(WLine);
      convertToUTF8(llvm::makeArrayRef(WLine, WLine + WLineLength),
                    CurrentLines);

      wcsncat(TotalLine, WLine, WLineLength);

      ++CurChunkLines;

      // If we detect a line starting with a colon, treat it as a special
      // REPL escape.
      char const *s = CurrentLines.data() + LineStart;
      char const *p = s;
      while (p < CurrentLines.end() && isspace(*p)) {
        ++p;
      }
      if (p == CurrentLines.end()) {
        if (!SCR.IsComplete) continue;
        return REPLInputKind::Empty;
      }

      if (CurChunkLines == 1 && SCR.IndentLevel == 0 && *p == ':') {
        // Colorize the response output.
        if (ShowColors)
          llvm::outs().changeColor(llvm::raw_ostream::GREEN);

        Result.clear();
        Result.append(CurrentLines.begin(), CurrentLines.end());

        // The lexer likes null-terminated data.
        Result.push_back('\0');
        Result.pop_back();

        // Enter the line into the line history.
        HistEventW ev;
        history_w(h, &ev, H_ENTER, TotalLine);

        return REPLInputKind::REPLDirective;
      }

      SCR = ide::isSourceInputComplete(CurrentLines.str(), SourceFileKind::REPL);
      // Keep reading if input is unfinished.
    } while (!SCR.IsComplete);

    // Enter the line into the line history.
    HistEventW ev;
    history_w(h, &ev, H_ENTER, TotalLine);

    Result.clear();
    Result.append(CurrentLines.begin(), CurrentLines.end());

    // The lexer likes null-terminated data.
    Result.push_back('\0');
    Result.pop_back();
    
    // Colorize the response output.
    if (ShowColors)
      llvm::outs().changeColor(llvm::raw_ostream::CYAN);
    
    return REPLInputKind::SourceCode;
  }

private:
  static wchar_t *PromptFn(EditLine *e) {
    void* clientdata;
    el_wget(e, EL_CLIENTDATA, &clientdata);
    return const_cast<wchar_t*>(((REPLInput*)clientdata)->getPrompt());
  }
  
  const wchar_t *getPrompt() {
    PromptString.clear();

    if (ShowColors) {
      const char *colorCode = llvm::sys::Process::OutputColor(
          static_cast<char>(llvm::raw_ostream::YELLOW), false, false);
      if (colorCode)
        appendEscapeSequence(PromptString, colorCode);
    }
    
    
    if (!NeedPromptContinuation)
      PromptString.insert(PromptString.end(), PS1, PS1 + wcslen(PS1));
    else {
      PromptString.insert(PromptString.end(), PS2, PS2 + wcslen(PS2));
      if (Autoindent)
        PromptString.append(2*PromptContinuationLevel, L' ');
    }
    
    if (ShowColors) {
      const char *colorCode = llvm::sys::Process::ResetColor();
      if (colorCode)
        appendEscapeSequence(PromptString, colorCode);
    }

    PromptedForLine = true;
    PromptString.push_back(L'\0');
    return PromptString.data();
  }

  /// Custom GETCFN to reset completion state after typing.
  static int GetCharFn(EditLine *e, wchar_t *out) {
    void* clientdata;
    el_wget(e, EL_CLIENTDATA, &clientdata);
    REPLInput *that = (REPLInput*)clientdata;

    wint_t c;
    while (errno = 0, (c = getwc(stdin)) == WEOF) {
      if (errno == EINTR)
        continue;
      *out = L'\0';
      return feof(stdin) ? 0 : -1;
    }

    // If the user typed anything other than tab, reset the completion state.
    if (c != L'\t') {
      that->completions.reset();
      that->CodeCompletionErasedBytes.clear();
    }
    *out = wchar_t(c);
    return 1;
  }
  
  template<unsigned char (REPLInput::*method)(int)>
  static unsigned char BindingFn(EditLine *e, int ch) {
    void *clientdata;
    el_wget(e, EL_CLIENTDATA, &clientdata);
    return (((REPLInput*)clientdata)->*method)(ch);
  }
  
  bool isAtStartOfLine(const LineInfoW *line) {
    for (wchar_t c : llvm::makeArrayRef(line->buffer,
                                        line->cursor - line->buffer)) {
      if (!iswspace(c))
        return false;
    }
    return true;
  }

  // /^\s*\w+\s*:$/
  bool lineLooksLikeLabel(const LineInfoW *line) {
    const wchar_t *p = line->buffer;
    while (p != line->cursor && iswspace(*p))
      ++p;

    if (p == line->cursor)
      return false;
    
    do {
      ++p;
    } while (p != line->cursor && (iswalnum(*p) || *p == L'_'));
    
    while (p != line->cursor && iswspace(*p))
      ++p;

    return p+1 == line->cursor || *p == L':';
  }
  
  // /^\s*set\s*\(.*\)\s*:$/
  bool lineLooksLikeSetter(const LineInfoW *line) {
    const wchar_t *p = line->buffer;
    while (p != line->cursor && iswspace(*p))
      ++p;
    
    if (p == line->cursor || *p++ != L's')
      return false;
    if (p == line->cursor || *p++ != L'e')
      return false;
    if (p == line->cursor || *p++ != L't')
      return false;

    while (p != line->cursor && iswspace(*p))
      ++p;
    
    if (p == line->cursor || *p++ != L'(')
      return false;
    
    if (line->cursor - p < 2 || line->cursor[-1] != L':')
      return false;

    p = line->cursor - 1;
    while (iswspace(*--p));

    return *p == L')';
  }
  
  // /^\s*case.*:$/
  bool lineLooksLikeCase(const LineInfoW *line) {
    const wchar_t *p = line->buffer;
    while (p != line->cursor && iswspace(*p))
      ++p;
    
    if (p == line->cursor || *p++ != L'c')
      return false;
    if (p == line->cursor || *p++ != L'a')
      return false;
    if (p == line->cursor || *p++ != L's')
      return false;
    if (p == line->cursor || *p++ != L'e')
      return false;
    
    return line->cursor[-1] == ':';
  }

  void outdent() {
    // If we didn't already outdent, do so.
    if (!Outdented) {
      if (PromptContinuationLevel > 0)
        --PromptContinuationLevel;
      Outdented = true;
    }
  }
  
  unsigned char onColon(int ch) {
    // Add the character to the string.
    wchar_t s[2] = {(wchar_t)ch, 0};
    el_winsertstr(e, s);
    
    const LineInfoW *line = el_wline(e);

    // Outdent if the line looks like a label.
    if (lineLooksLikeLabel(line))
      outdent();
    // Outdent if the line looks like a setter.
    else if (lineLooksLikeSetter(line))
      outdent();
    // Outdent if the line looks like a 'case' label.
    else if (lineLooksLikeCase(line))
      outdent();
    
    return CC_REFRESH;
  }
  
  unsigned char onCloseBrace(int ch) {
    bool atStart = isAtStartOfLine(el_wline(e));
    
    // Add the character to the string.
    wchar_t s[2] = {(wchar_t)ch, 0};
    el_winsertstr(e, s);
    
    // Don't outdent if we weren't at the start of the line.
    if (!atStart) {
      return CC_REFRESH;
    }
    
    outdent();
    return CC_REFRESH;
  }
  
  unsigned char onIndentOrComplete(int ch) {
    const LineInfoW *line = el_wline(e);
    
    // FIXME: UTF-8? What's that?
    size_t cursorPos = line->cursor - line->buffer;
    
    // If there's nothing but whitespace before the cursor, indent to the next
    // 2-character tab stop.
    if (isAtStartOfLine(line)) {
      const wchar_t *indent = cursorPos & 1 ? L" " : L"  ";
      el_winsertstr(e, indent);
      return CC_REFRESH;
    }
    
    // Otherwise, look for completions.
    return onComplete(ch);
  }
  
  void insertStringRef(StringRef s) {
    if (s.empty())
      return;
    // Convert s to wchar_t* and null-terminate for el_winsertstr.
    SmallVector<wchar_t, 64> TmpStr;
    convertFromUTF8(s, TmpStr);
    TmpStr.push_back(L'\0');
    el_winsertstr(e, TmpStr.data());
  }
  
  void displayCompletions(llvm::ArrayRef<llvm::StringRef> list) {
    // FIXME: Do the print-completions-below-the-prompt thing bash does.
    llvm::outs() << '\n';
    // Trim the completion list to the terminal size.
    int lines_int = 0, columns_int = 0;
    // NB: EL_GETTC doesn't work with el_wget (?!)
    el_get(e, EL_GETTC, "li", &lines_int);
    el_get(e, EL_GETTC, "co", &columns_int);
    assert(lines_int > 0 && columns_int > 0 && "negative or zero screen size?!");
    
    auto lines = size_t(lines_int), columns = size_t(columns_int);
    size_t trimToColumns = columns > 2 ? columns - 2 : 0;
    
    size_t trimmed = 0;
    if (list.size() > lines - 1) {
      size_t trimToLines = lines > 2 ? lines - 2 : 0;
      trimmed = list.size() - trimToLines;
      list = list.slice(0, trimToLines);
    }
    
    for (StringRef completion : list) {
      if (completion.size() > trimToColumns)
        completion = completion.slice(0, trimToColumns);
      llvm::outs() << "  " << completion << '\n';
    }
    if (trimmed > 0)
      llvm::outs() << "  (and " << trimmed << " more)\n";
  }

  SourceFile &getFileForCodeCompletion();
  
  unsigned char onComplete(int ch) {
    const LineInfoW *line = el_wline(e);
    llvm::ArrayRef<wchar_t> wprefix(line->buffer, line->cursor - line->buffer);
    llvm::SmallString<64> Prefix;
    Prefix.assign(CurrentLines);
    convertToUTF8(wprefix, Prefix);
    
    if (!completions) {
      // If we aren't currently working with a completion set, generate one.
      completions.populate(getFileForCodeCompletion(), Prefix);
      // Display the common root of the found completions and beep unless we
      // found a unique one.
      insertStringRef(completions.getRoot());
      return completions.isUnique()
        ? CC_REFRESH
        : CC_REFRESH_BEEP;
    }
    
    // Otherwise, advance through the completion state machine.
    switch (completions.getState()) {
    case CompletionState::CompletedRoot:
      // We completed the root. Next step is to display the completion list.
      displayCompletions(completions.getCompletionList());
      completions.setState(CompletionState::DisplayedCompletionList);
      return CC_REDISPLAY;

    case CompletionState::DisplayedCompletionList: {
      // Complete the next completion stem in the cycle.
      const auto Last = completions.getPreviousStem();
      el_wdeletestr(e, Last.InsertableString.size());
      Prefix.resize(Prefix.size() - Last.InsertableString.size());
      insertStringRef(CodeCompletionErasedBytes);
      Prefix.append(CodeCompletionErasedBytes);

      const auto Next = completions.getNextStem();
      CodeCompletionErasedBytes.clear();
      if (Next.NumBytesToErase != 0) {
        CodeCompletionErasedBytes.assign(Prefix.end() - Next.NumBytesToErase, Prefix.end());
        el_wdeletestr(e, Next.NumBytesToErase);
      }
      insertStringRef(Next.InsertableString);
      return CC_REFRESH;
    }
    
    case CompletionState::Empty:
    case CompletionState::Unique:
      // We already provided a definitive completion--nothing else to do.
      return CC_REFRESH_BEEP;

    case CompletionState::Invalid:
      llvm_unreachable("got an invalid completion set?!");
    }
  }
};

enum class PrintOrDump { Print, Dump };

static void printOrDumpDecl(Decl *d, PrintOrDump which) {
  if (which == PrintOrDump::Print) {
    d->print(llvm::outs());
    llvm::outs() << '\n';
  } else
    d->dump(llvm::outs());
}

/// The compiler and execution environment for the REPL.
class REPLEnvironment {
  std::unique_ptr<llvm::LLVMContext> LLVMContext;
  CompilerInstance &CI;
  ModuleDecl *MostRecentModule;
  ProcessCmdLine CmdLine;
  llvm::SmallPtrSet<swift::ModuleDecl *, 8> ImportedModules;
  SmallVector<llvm::Function*, 8> InitFns;
  bool RanGlobalInitializers;
  llvm::Module *Module;
  llvm::StringSet<> FuncsAlreadyGenerated;
  llvm::StringSet<> GlobalsAlreadyEmitted;
  llvm::Module DumpModule;
  llvm::SmallString<128> DumpSource;

  llvm::ExecutionEngine *EE;
  IRGenOptions IRGenOpts;
  const SILOptions SILOpts;

  REPLInput Input;
  unsigned NextLineNumber = 0;

private:

  void stripPreviouslyGenerated(llvm::Module &M) {
    for (auto &function : M.getFunctionList()) {
      function.setVisibility(llvm::GlobalValue::DefaultVisibility);
      if (FuncsAlreadyGenerated.count(function.getName()))
        function.deleteBody();
      else {
        if (function.getName() != SWIFT_ENTRY_POINT_FUNCTION)
          FuncsAlreadyGenerated.insert(function.getName());
      }
    }

    for (auto &global : M.globals()) {
      if (!global.hasName())
        continue;
      if (global.hasGlobalUnnamedAddr())
        continue;

      global.setVisibility(llvm::GlobalValue::DefaultVisibility);
      if (!global.hasAvailableExternallyLinkage() &&
          !global.hasAppendingLinkage() &&
          !global.hasCommonLinkage()) {
        if (GlobalsAlreadyEmitted.count(global.getName())) {
          // Some targets don't support relative references to undefined
          // symbols. Keep the local copy of an ODR symbol if it's used in
          // a relative reference expression.
          bool usedInRelativeReference = false;
          if (global.hasLinkOnceODRLinkage()) {
            
            for (auto user : global.users()) {
              // A relative reference will look like sub (ptrtoint @Global, _)
              auto expr = dyn_cast<llvm::ConstantExpr>(user);
              if (!expr)
                continue;
              
              if (expr->getOpcode() != llvm::Instruction::PtrToInt)
                continue;
              
              for (auto exprUser : expr->users()) {
                auto exprExpr = dyn_cast<llvm::ConstantExpr>(exprUser);
                if (!exprExpr)
                  continue;
                
                if (exprExpr->getOpcode() != llvm::Instruction::Sub)
                  continue;
                
                if (exprExpr->getOperand(0) != expr)
                  continue;
                
                usedInRelativeReference = true;
                goto done;
              }
              
            }
          }
        done:
          if (!usedInRelativeReference)
            global.setInitializer(nullptr);
        } else
          GlobalsAlreadyEmitted.insert(global.getName());

        global.setLinkage(llvm::GlobalValue::ExternalLinkage);
      }
    }


    for (auto alias = M.alias_begin(); alias != M.alias_end();) {
      alias->setVisibility(llvm::GlobalValue::DefaultVisibility);
      if (!alias->hasAvailableExternallyLinkage() &&
          !alias->hasAppendingLinkage() &&
          !alias->hasCommonLinkage()) {
        alias->setLinkage(llvm::GlobalValue::ExternalLinkage);
        if (GlobalsAlreadyEmitted.count(alias->getName())) {
          // Replace already-emitted aliases with external declarations.
          SmallString<32> name = alias->getName();
          alias->setName("");
          auto external = new llvm::GlobalVariable(
            M,
            alias->getType()->getPointerElementType(),
            /*isConstant*/ false,
            alias->getLinkage(),
            /*initializer*/ nullptr,
            name);
          alias->replaceAllUsesWith(external);
          auto &aliasToRemove = *alias;
          ++alias;
          aliasToRemove.eraseFromParent();
        } else {
          GlobalsAlreadyEmitted.insert(alias->getName());
          ++alias;
        }
      }
    }
  }

  bool linkLLVMModules(llvm::Module *Module,
                       std::unique_ptr<llvm::Module> &&ModuleToLink) {
    // EGREGIOUS HACKS AHEAD
    //
    // 1) LLVMContext does not support robust notions of identity rdar://61895075
    // 2) llvm::Linker does not support modules allocated in separate contexts
    //    rdar://61894890
    // 3) Cloning modules across contexts is a known deficiency in LLVM
    //    rdar://61896692
    // 4) Round-tripping through the IR printer and parser is not guaranteed to
    //    result in the same IR. It is heavily tested and implied that this is
    //    the case, but LLVM makes no formal guarantees.
    //
    // So, work around 1) and do a naive pointer comparison to see if we
    // allocated the module we're about to link in a separate context.
    if (&Module->getContext() != &ModuleToLink->getContext()) {
      // If so, work around 2) by round-tripping the IR and parsing back it
      // into the original context. With module in-hand, we have successfully
      // worked around 3).
      llvm::SmallString<1024> scratch;
      llvm::raw_svector_ostream PrintBuffer(scratch);
      ModuleToLink->print(PrintBuffer, nullptr);
      auto Buffer = llvm::MemoryBuffer::getMemBufferCopy(PrintBuffer.str());
      llvm::SMDiagnostic Err;
      // Finally, hope that 4) doesn't come back to bite us in the long run.
      ModuleToLink = llvm::parseIR(Buffer->getMemBufferRef(), Err,
                                   Module->getContext());
    }

    llvm::LLVMContext &Ctx = Module->getContext();
    auto OldHandler = Ctx.getDiagnosticHandlerCallBack();
    void *OldDiagnosticContext = Ctx.getDiagnosticContext();
    Ctx.setDiagnosticHandlerCallBack(linkerDiagnosticHandler, nullptr);
    bool Failed = llvm::Linker::linkModules(*Module, std::move(ModuleToLink));
    Ctx.setDiagnosticHandlerCallBack(OldHandler, OldDiagnosticContext);
    return !Failed;
  }

  bool executeSwiftSource(llvm::StringRef Line, const ProcessCmdLine &CmdLine) {
    SWIFT_DEFER {
      // Always flush diagnostic consumers after executing a line.
      CI.getDiags().flushConsumers();
    };

    // Parse the current line(s).
    auto InputBuf = llvm::MemoryBuffer::getMemBufferCopy(Line, "<REPL Input>");
    SmallString<8> Name{"REPL_"};
    llvm::raw_svector_ostream(Name) << NextLineNumber;
    ++NextLineNumber;
    ModuleDecl *M = typeCheckREPLInput(MostRecentModule, Name,
                                       std::move(InputBuf));
    
    // SILGen the module and produce SIL diagnostics.
    std::unique_ptr<SILModule> sil;
    
    if (!CI.getASTContext().hadError()) {
      // We don't want anything to get stripped, so pretend we're doing a
      // non-whole-module generation.
      sil = performSILGeneration(*M->getFiles().front(), CI.getSILTypes(),
                                 CI.getSILOptions());
      runSILDiagnosticPasses(*sil);
      runSILOwnershipEliminatorPass(*sil);
      runSILLoweringPasses(*sil);
    }

    if (CI.getASTContext().hadError()) {
      if (CI.getDiags().hasFatalErrorOccurred())
        return false;

      CI.getASTContext().Diags.resetHadAnyError();

      // FIXME: Handling of "import" declarations?  Is there any other
      // state which needs to be reset?
      
      return true;
    }
    MostRecentModule = M;

    DumpSource += Line;

    // IRGen the current line(s).
    auto GenModule = performIRGeneration(
        IRGenOpts, M, std::move(sil), "REPLLine", PrimarySpecificPaths(),
        /*parallelOutputFilenames*/{});

    if (CI.getASTContext().hadError())
      return false;

    assert(GenModule && "Emitted no diagnostics but IR generation failed?");

    // Release the module and context because we are about to clone another
    // module into the context which violates the exclusivity guarantees of
    // GeneratedModule.
    llvm::LLVMContext *Ctx;
    llvm::Module *ModuleToLink;
    std::tie(Ctx, ModuleToLink) = std::move(GenModule).release();

    // LineModule will get destroy by the following link process.
    // Make a copy of it to be able to correct produce DumpModule.
    std::unique_ptr<llvm::Module> SaveLineModule(CloneModule(*ModuleToLink));
    
    if (!linkLLVMModules(Module, std::unique_ptr<llvm::Module>(ModuleToLink))) {
      return false;
    }

    std::unique_ptr<llvm::Module> NewModule(CloneModule(*Module));

    Module->getFunction("main")->eraseFromParent();

    stripPreviouslyGenerated(*NewModule);

    if (!linkLLVMModules(&DumpModule, std::move(SaveLineModule))) {
      return false;
    }
    llvm::Function *DumpModuleMain = DumpModule.getFunction("main");
    DumpModuleMain->setName("repl.line");
    
    if (autolinkImportedModules(M, IRGenOpts))
      return false;
    
    llvm::Module *TempModule = NewModule.get();
    EE->addModule(std::move(NewModule));

    EE->finalizeObject();

    for (auto InitFn : InitFns)
      EE->runFunctionAsMain(InitFn, CmdLine, nullptr);
    InitFns.clear();
    
    // FIXME: The way we do this is really ugly... we should be able to
    // improve this.
    if (!RanGlobalInitializers) {
      EE->runStaticConstructorsDestructors(*TempModule, false);
      RanGlobalInitializers = true;
    }
    llvm::Function *EntryFn = TempModule->getFunction("main");
    EE->runFunctionAsMain(EntryFn, CmdLine, nullptr);

    // Clean up the code generation context now that we're done cloning modules.
    delete Ctx;

    return true;
  }

public:
  REPLEnvironment(CompilerInstance &CI,
                  const ProcessCmdLine &CmdLine,
                  bool ParseStdlib)
    : LLVMContext(std::make_unique<llvm::LLVMContext>()),
      CI(CI),
      MostRecentModule(CI.getMainModule()),
      CmdLine(CmdLine),
      RanGlobalInitializers(false),
      Module(new llvm::Module("REPL", *LLVMContext.get())),
      DumpModule("REPL", *LLVMContext.get()),
      IRGenOpts(),
      SILOpts(),
      Input(*this)
  {
    ASTContext &Ctx = CI.getASTContext();
    Ctx.LangOpts.EnableAccessControl = false;
    if (!ParseStdlib) {
      if (!loadSwiftRuntime(Ctx.SearchPathOpts.RuntimeLibraryPaths)) {
        CI.getDiags().diagnose(SourceLoc(),
                               diag::error_immediate_mode_missing_stdlib);
        return;
      }
      tryLoadLibraries(CI.getLinkLibraries(), Ctx.SearchPathOpts,
                       CI.getDiags());
    }

    llvm::EngineBuilder builder{std::unique_ptr<llvm::Module>{Module}};
    std::string ErrorMsg;
    llvm::TargetOptions TargetOpt;
    std::string CPU;
    std::string Triple;
    std::vector<std::string> Features;
    std::tie(TargetOpt, CPU, Features, Triple)
      = getIRTargetOptions(IRGenOpts, CI.getASTContext());
    
    builder.setRelocationModel(llvm::Reloc::PIC_);
    builder.setTargetOptions(TargetOpt);
    builder.setMCPU(CPU);
    builder.setMAttrs(Features);
    builder.setErrorStr(&ErrorMsg);
    builder.setEngineKind(llvm::EngineKind::JIT);
    EE = builder.create();

    IRGenOpts.OptMode = OptimizationMode::NoOptimization;
    IRGenOpts.OutputKind = IRGenOutputKind::Module;
    IRGenOpts.UseJIT = true;
    IRGenOpts.IntegratedREPL = true;
    IRGenOpts.DebugInfoLevel = IRGenDebugInfoLevel::None;
    IRGenOpts.DebugInfoFormat = IRGenDebugInfoFormat::None;

    if (!ParseStdlib) {
      // Force standard library to be loaded immediately.  This forces any
      // errors to appear upfront, and helps eliminate some nasty lag after the
      // first statement is typed into the REPL.
      static const char WarmUpStmt[] = "Void()\n";

      auto Buffer =
          llvm::MemoryBuffer::getMemBufferCopy(WarmUpStmt,
                                               "<REPL Initialization>");
      (void)typeCheckREPLInput(MostRecentModule, "__Warmup", std::move(Buffer));

      if (Ctx.hadError())
        return;
    }

    if (llvm::sys::Process::StandardInIsUserInput())
      llvm::outs() <<
          "***  You are running Swift's integrated REPL,  ***\n"
          "***  intended for compiler and stdlib          ***\n"
          "***  development and testing purposes only.    ***\n"
          "***  The full REPL is built as part of LLDB.   ***\n"
          "***  Type ':help' for assistance.              ***\n";
  }
  
  StringRef getDumpSource() const { return DumpSource; }
  
  /// Get the REPLInput object owned by the REPL instance.
  REPLInput &getInput() { return Input; }

  SourceFile &getFileForCodeCompletion() {
    return MostRecentModule->getMainSourceFile(SourceFileKind::REPL);
  }
  
  /// Responds to a REPL input. Returns true if the repl should continue,
  /// false if it should quit.
  bool handleREPLInput(REPLInputKind inputKind, llvm::StringRef Line) {
    switch (inputKind) {
      case REPLInputKind::REPLQuit:
        return false;
        
      case REPLInputKind::Empty:
        return true;
        
      case REPLInputKind::REPLDirective: {
        unsigned BufferID =
            CI.getSourceMgr().addMemBufferCopy(Line, "<REPL Input>");
        Lexer L(CI.getASTContext().LangOpts,
                CI.getSourceMgr(), BufferID, nullptr, LexerMode::Swift);
        Token Tok;
        L.lex(Tok);
        assert(Tok.is(tok::colon));
        
        if (L.peekNextToken().getText() == "help") {
          llvm::outs() << "Available commands:\n"
               "  :quit - quit the interpreter (you can also use :exit "
                   "or Control+D or exit(0))\n"
               "  :autoindent (on|off) - turn on/off automatic indentation of"
                   " bracketed lines\n"
               "  :constraints debug (on|off) - turn on/off the debug "
                   "output for the constraint-based type checker\n"
               "  :dump_ir - dump the LLVM IR generated by the REPL\n"
               "  :dump_decl <name> - dump the AST representation of the "
                   "named declarations\n"
               "  :dump_source - dump the user input (ignoring"
                   " lines with errors)\n"
               "  :print_decl <name> - print the AST representation of the "
                   "named declarations\n"
               "  :print_module <name> - print the decls in the given "
                   "module, but not submodules\n"
               "API documentation etc. will be here eventually.\n";
        } else if (L.peekNextToken().getText() == "quit" ||
                   L.peekNextToken().getText() == "exit") {
          return false;
        } else if (L.peekNextToken().getText() == "dump_ir") {
          DumpModule.print(llvm::dbgs(), nullptr, false, true);
        } else if (L.peekNextToken().getText() == "dump_decl" ||
                   L.peekNextToken().getText() == "print_decl") {
          PrintOrDump doPrint = (L.peekNextToken().getText() == "print_decl")
            ? PrintOrDump::Print : PrintOrDump::Dump;
          L.lex(Tok);
          L.lex(Tok);
          ASTContext &ctx = CI.getASTContext();
          SourceFile &SF =
              MostRecentModule->getMainSourceFile(SourceFileKind::REPL);
          DeclNameRef name(ctx.getIdentifier(Tok.getText()));
          auto descriptor = UnqualifiedLookupDescriptor(name, &SF);
          auto lookup = evaluateOrDefault(
              ctx.evaluator, UnqualifiedLookupRequest{descriptor}, {});
          for (auto result : lookup) {
            printOrDumpDecl(result.getValueDecl(), doPrint);
              
            if (auto typeDecl = dyn_cast<TypeDecl>(result.getValueDecl())) {
              if (auto typeAliasDecl = dyn_cast<TypeAliasDecl>(typeDecl)) {
                TypeDecl *origTypeDecl = typeAliasDecl
                  ->getDeclaredInterfaceType()
                  ->getNominalOrBoundGenericNominal();
                if (origTypeDecl) {
                  printOrDumpDecl(origTypeDecl, doPrint);
                  typeDecl = origTypeDecl;
                }
              }

              // Print extensions.
              if (auto nominal = dyn_cast<NominalTypeDecl>(typeDecl)) {
                for (auto extension : nominal->getExtensions()) {
                  printOrDumpDecl(extension, doPrint);
                }
              }
            }
          }
        } else if (L.peekNextToken().getText() == "dump_source") {
          llvm::errs() << DumpSource;
        } else if (L.peekNextToken().getText() == "print_module") {
          L.lex(Tok);
          SmallVector<ImportDecl::AccessPathElement, 4> accessPath;
          ASTContext &ctx = CI.getASTContext();

          L.lex(Tok);
          if (Tok.is(tok::identifier)) {
            accessPath.push_back({ctx.getIdentifier(Tok.getText()),
                                  Tok.getLoc()});
            
            while (L.peekNextToken().is(tok::period)) {
              L.lex(Tok);
              L.lex(Tok);
              if (Tok.is(tok::identifier)) {
                accessPath.push_back({ctx.getIdentifier(Tok.getText()),
                                      Tok.getLoc()});
              } else {
                llvm::outs() << "Not a submodule name: '" << Tok.getText()
                             << "'\n";
                accessPath.clear();
              }
            }
          } else {
            llvm::outs() << "Not a module name: '" << Tok.getText() << "'\n";
          }
          
          if (!accessPath.empty()) {
            auto M = ctx.getModule(accessPath);
            if (!M)
              llvm::outs() << "No such module\n";
            else {
              SmallVector<Decl *, 64> decls;
              M->getDisplayDecls(decls);
              for (const Decl *D : decls) {
                D->print(llvm::outs());
                llvm::outs() << '\n';
              }
            }
          }
          
        } else if (L.peekNextToken().getText() == "constraints") {
          L.lex(Tok);
          L.lex(Tok);
          if (Tok.getText() == "debug") {
            L.lex(Tok);
            if (Tok.getText() == "on") {
              CI.getASTContext().TypeCheckerOpts.DebugConstraintSolver = true;
            } else if (Tok.getText() == "off") {
              CI.getASTContext().TypeCheckerOpts.DebugConstraintSolver = false;
            } else {
              llvm::outs() << "Unknown :constraints debug command; try :help\n";
            }
          } else {
            llvm::outs() << "Unknown :constraints command; try :help\n";
          }
        } else if (L.peekNextToken().getText() == "autoindent") {
          L.lex(Tok);
          L.lex(Tok);
          if (Tok.getText() == "on") {
            Input.Autoindent = true;
          } else if (Tok.getText() == "off") {
            Input.Autoindent = false;
          } else {
            llvm::outs() << "Unknown :autoindent command; try :help\n";
          }
        } else {
          llvm::outs() << "Unknown interpreter escape; try :help\n";
        }
        return true;
      }
        
      case REPLInputKind::SourceCode: {
        // Execute this source line.
        return executeSwiftSource(Line, CmdLine);
      }
    }
  }
};

inline SourceFile &REPLInput::getFileForCodeCompletion() {
  return Env.getFileForCodeCompletion();
}

void PrettyStackTraceREPL::print(llvm::raw_ostream &out) const {
  out << "while processing REPL source:\n";
  out << Input.Env.getDumpSource();
  llvm::outs().resetColor();
  llvm::errs().resetColor();
}

void swift::runREPL(CompilerInstance &CI, const ProcessCmdLine &CmdLine,
                    bool ParseStdlib) {
  REPLEnvironment env(CI, CmdLine, ParseStdlib);
  if (CI.getASTContext().hadError())
    return;
  
  llvm::SmallString<80> Line;
  REPLInputKind inputKind;
  do {
    inputKind = env.getInput().getREPLInput(Line);
  } while (env.handleREPLInput(inputKind, Line));
}

#else

void swift::runREPL(CompilerInstance &CI, const ProcessCmdLine &CmdLine,
                    bool ParseStdlib) {
  // Disable the REPL on other platforms; our current implementation is tied
  // to histedit.h.
  llvm::report_fatal_error("Compiler-internal integrated REPL unimplemented "
                           "for this platform; use the LLDB-enhanced REPL "
                           "instead.");
}
#endif
