//===- DependencyDirectivesSourceMinimizer.cpp -  -------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This is the implementation for minimizing header and source files to the
/// minimum necessary preprocessor directives for evaluating includes. It
/// reduces the source down to #define, #include, #import, @import, and any
/// conditional preprocessor logic that contains one of those.
///
//===----------------------------------------------------------------------===//

#include "clang/Lex/DependencyDirectivesSourceMinimizer.h"
#include "clang/Basic/CharInfo.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Lex/LexDiagnostic.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/MemoryBuffer.h"

using namespace llvm;
using namespace clang;
using namespace clang::minimize_source_to_dependency_directives;

namespace {

struct Minimizer {
  /// Minimized output.
  SmallVectorImpl<char> &Out;
  /// The known tokens encountered during the minimization.
  SmallVectorImpl<Token> &Tokens;

  Minimizer(SmallVectorImpl<char> &Out, SmallVectorImpl<Token> &Tokens,
            StringRef Input, DiagnosticsEngine *Diags,
            SourceLocation InputSourceLoc)
      : Out(Out), Tokens(Tokens), Input(Input), Diags(Diags),
        InputSourceLoc(InputSourceLoc) {}

  /// Lex the provided source and emit the minimized output.
  ///
  /// \returns True on error.
  bool minimize();

private:
  struct IdInfo {
    const char *Last;
    StringRef Name;
  };

  /// Lex an identifier.
  ///
  /// \pre First points at a valid identifier head.
  LLVM_NODISCARD IdInfo lexIdentifier(const char *First, const char *const End);
  LLVM_NODISCARD bool isNextIdentifier(StringRef Id, const char *&First,
                                       const char *const End);
  LLVM_NODISCARD bool minimizeImpl(const char *First, const char *const End);
  LLVM_NODISCARD bool lexPPLine(const char *&First, const char *const End);
  LLVM_NODISCARD bool lexAt(const char *&First, const char *const End);
  LLVM_NODISCARD bool lexDefine(const char *&First, const char *const End);
  LLVM_NODISCARD bool lexPragma(const char *&First, const char *const End);
  LLVM_NODISCARD bool lexEndif(const char *&First, const char *const End);
  LLVM_NODISCARD bool lexDefault(TokenKind Kind, StringRef Directive,
                                 const char *&First, const char *const End);
  Token &makeToken(TokenKind K) {
    Tokens.emplace_back(K, Out.size());
    return Tokens.back();
  }
  void popToken() {
    Out.resize(Tokens.back().Offset);
    Tokens.pop_back();
  }
  TokenKind top() const { return Tokens.empty() ? pp_none : Tokens.back().K; }

  Minimizer &put(char Byte) {
    Out.push_back(Byte);
    return *this;
  }
  Minimizer &append(StringRef S) { return append(S.begin(), S.end()); }
  Minimizer &append(const char *First, const char *Last) {
    Out.append(First, Last);
    return *this;
  }

  void printToNewline(const char *&First, const char *const End);
  void printAdjacentModuleNameParts(const char *&First, const char *const End);
  LLVM_NODISCARD bool printAtImportBody(const char *&First,
                                        const char *const End);
  void printDirectiveBody(const char *&First, const char *const End);
  void printAdjacentMacroArgs(const char *&First, const char *const End);
  LLVM_NODISCARD bool printMacroArgs(const char *&First, const char *const End);

  /// Reports a diagnostic if the diagnostic engine is provided. Always returns
  /// true at the end.
  bool reportError(const char *CurPtr, unsigned Err);

  StringMap<char> SplitIds;
  StringRef Input;
  DiagnosticsEngine *Diags;
  SourceLocation InputSourceLoc;
};

} // end anonymous namespace

bool Minimizer::reportError(const char *CurPtr, unsigned Err) {
  if (!Diags)
    return true;
  assert(CurPtr >= Input.data() && "invalid buffer ptr");
  Diags->Report(InputSourceLoc.getLocWithOffset(CurPtr - Input.data()), Err);
  return true;
}

static void skipOverSpaces(const char *&First, const char *const End) {
  while (First != End && isHorizontalWhitespace(*First))
    ++First;
}

LLVM_NODISCARD static bool isRawStringLiteral(const char *First,
                                              const char *Current) {
  assert(First <= Current);

  // Check if we can even back up.
  if (*Current != '"' || First == Current)
    return false;

  // Check for an "R".
  --Current;
  if (*Current != 'R')
    return false;
  if (First == Current || !isIdentifierBody(*--Current))
    return true;

  // Check for a prefix of "u", "U", or "L".
  if (*Current == 'u' || *Current == 'U' || *Current == 'L')
    return First == Current || !isIdentifierBody(*--Current);

  // Check for a prefix of "u8".
  if (*Current != '8' || First == Current || *Current-- != 'u')
    return false;
  return First == Current || !isIdentifierBody(*--Current);
}

static void skipRawString(const char *&First, const char *const End) {
  assert(First[0] == '"');
  assert(First[-1] == 'R');

  const char *Last = ++First;
  while (Last != End && *Last != '(')
    ++Last;
  if (Last == End) {
    First = Last; // Hit the end... just give up.
    return;
  }

  StringRef Terminator(First, Last - First);
  for (;;) {
    // Move First to just past the next ")".
    First = Last;
    while (First != End && *First != ')')
      ++First;
    if (First == End)
      return;
    ++First;

    // Look ahead for the terminator sequence.
    Last = First;
    while (Last != End && size_t(Last - First) < Terminator.size() &&
           Terminator[Last - First] == *Last)
      ++Last;

    // Check if we hit it (or the end of the file).
    if (Last == End) {
      First = Last;
      return;
    }
    if (size_t(Last - First) < Terminator.size())
      continue;
    if (*Last != '"')
      continue;
    First = Last + 1;
    return;
  }
}

static void skipString(const char *&First, const char *const End) {
  assert(*First == '\'' || *First == '"');
  const char Terminator = *First;
  for (++First; First != End && *First != Terminator; ++First)
    if (*First == '\\')
      if (++First == End)
        return;
  if (First != End)
    ++First; // Finish off the string.
}

static void skipNewline(const char *&First, const char *End) {
  assert(isVerticalWhitespace(*First));
  ++First;
  if (First == End)
    return;

  // Check for "\n\r" and "\r\n".
  if (LLVM_UNLIKELY(isVerticalWhitespace(*First) && First[-1] != First[0]))
    ++First;
}

static void skipToNewlineRaw(const char *&First, const char *const End) {
  for (;;) {
    if (First == End)
      return;

    if (isVerticalWhitespace(*First))
      return;

    while (!isVerticalWhitespace(*First))
      if (++First == End)
        return;

    if (First[-1] != '\\')
      return;

    ++First; // Keep going...
  }
}

static const char *reverseOverSpaces(const char *First, const char *Last) {
  assert(First <= Last);
  while (First != Last && isHorizontalWhitespace(Last[-1]))
    --Last;
  return Last;
}

static void skipLineComment(const char *&First, const char *const End) {
  assert(First[0] == '/' && First[1] == '/');
  First += 2;
  skipToNewlineRaw(First, End);
}

static void skipBlockComment(const char *&First, const char *const End) {
  assert(First[0] == '/' && First[1] == '*');
  if (End - First < 4) {
    First = End;
    return;
  }
  for (First += 3; First != End; ++First)
    if (First[-1] == '*' && First[0] == '/') {
      ++First;
      return;
    }
}

/// \returns True if the current single quotation mark character is a C++ 14
/// digit separator.
static bool isQuoteCppDigitSeparator(const char *const Start,
                                     const char *const Cur,
                                     const char *const End) {
  assert(*Cur == '\'' && "expected quotation character");
  // skipLine called in places where we don't expect a valid number
  // body before `start` on the same line, so always return false at the start.
  if (Start == Cur)
    return false;
  // The previous character must be a valid PP number character.
  // Make sure that the L, u, U, u8 prefixes don't get marked as a
  // separator though.
  char Prev = *(Cur - 1);
  if (Prev == 'L' || Prev == 'U' || Prev == 'u')
    return false;
  if (Prev == '8' && (Cur - 1 != Start) && *(Cur - 2) == 'u')
    return false;
  if (!isPreprocessingNumberBody(Prev))
    return false;
  // The next character should be a valid identifier body character.
  return (Cur + 1) < End && isIdentifierBody(*(Cur + 1));
}

static void skipLine(const char *&First, const char *const End) {
  do {
    assert(First <= End);
    if (First == End)
      return;

    if (isVerticalWhitespace(*First)) {
      skipNewline(First, End);
      return;
    }
    const char *Start = First;
    while (First != End && !isVerticalWhitespace(*First)) {
      // Iterate over strings correctly to avoid comments and newlines.
      if (*First == '"' ||
          (*First == '\'' && !isQuoteCppDigitSeparator(Start, First, End))) {
        if (isRawStringLiteral(Start, First))
          skipRawString(First, End);
        else
          skipString(First, End);
        continue;
      }

      // Iterate over comments correctly.
      if (*First != '/' || End - First < 2) {
        ++First;
        continue;
      }

      if (First[1] == '/') {
        // "//...".
        skipLineComment(First, End);
        continue;
      }

      if (First[1] != '*') {
        ++First;
        continue;
      }

      // "/*...*/".
      skipBlockComment(First, End);
    }
    if (First == End)
      return;

    // Skip over the newline.
    assert(isVerticalWhitespace(*First));
    skipNewline(First, End);
  } while (First[-2] == '\\'); // Continue past line-continuations.
}

static void skipDirective(StringRef Name, const char *&First,
                          const char *const End) {
  if (llvm::StringSwitch<bool>(Name)
          .Case("warning", true)
          .Case("error", true)
          .Default(false))
    // Do not process quotes or comments.
    skipToNewlineRaw(First, End);
  else
    skipLine(First, End);
}

void Minimizer::printToNewline(const char *&First, const char *const End) {
  while (First != End && !isVerticalWhitespace(*First)) {
    const char *Last = First;
    do {
      // Iterate over strings correctly to avoid comments and newlines.
      if (*Last == '"' || *Last == '\'') {
        if (LLVM_UNLIKELY(isRawStringLiteral(First, Last)))
          skipRawString(Last, End);
        else
          skipString(Last, End);
        continue;
      }
      if (*Last != '/' || End - Last < 2) {
        ++Last;
        continue; // Gather the rest up to print verbatim.
      }

      if (Last[1] != '/' && Last[1] != '*') {
        ++Last;
        continue;
      }

      // Deal with "//..." and "/*...*/".
      append(First, reverseOverSpaces(First, Last));
      First = Last;

      if (Last[1] == '/') {
        skipLineComment(First, End);
        return;
      }

      put(' ');
      skipBlockComment(First, End);
      skipOverSpaces(First, End);
      Last = First;
    } while (Last != End && !isVerticalWhitespace(*Last));

    // Print out the string.
    if (Last == End || Last == First || Last[-1] != '\\') {
      append(First, reverseOverSpaces(First, Last));
      return;
    }

    // Print up to the backslash, backing up over spaces.
    append(First, reverseOverSpaces(First, Last - 1));

    First = Last;
    skipNewline(First, End);
    skipOverSpaces(First, End);
  }
}

static void skipWhitespace(const char *&First, const char *const End) {
  for (;;) {
    assert(First <= End);
    skipOverSpaces(First, End);

    if (End - First < 2)
      return;

    if (First[0] == '\\' && isVerticalWhitespace(First[1])) {
      skipNewline(++First, End);
      continue;
    }

    // Check for a non-comment character.
    if (First[0] != '/')
      return;

    // "// ...".
    if (First[1] == '/') {
      skipLineComment(First, End);
      return;
    }

    // Cannot be a comment.
    if (First[1] != '*')
      return;

    // "/*...*/".
    skipBlockComment(First, End);
  }
}

void Minimizer::printAdjacentModuleNameParts(const char *&First,
                                             const char *const End) {
  // Skip over parts of the body.
  const char *Last = First;
  do
    ++Last;
  while (Last != End && (isIdentifierBody(*Last) || *Last == '.'));
  append(First, Last);
  First = Last;
}

bool Minimizer::printAtImportBody(const char *&First, const char *const End) {
  for (;;) {
    skipWhitespace(First, End);
    if (First == End)
      return true;

    if (isVerticalWhitespace(*First)) {
      skipNewline(First, End);
      continue;
    }

    // Found a semicolon.
    if (*First == ';') {
      put(*First++).put('\n');
      return false;
    }

    // Don't handle macro expansions inside @import for now.
    if (!isIdentifierBody(*First) && *First != '.')
      return true;

    printAdjacentModuleNameParts(First, End);
  }
}

void Minimizer::printDirectiveBody(const char *&First, const char *const End) {
  skipWhitespace(First, End); // Skip initial whitespace.
  printToNewline(First, End);
  while (Out.back() == ' ')
    Out.pop_back();
  put('\n');
}

LLVM_NODISCARD static const char *lexRawIdentifier(const char *First,
                                                   const char *const End) {
  assert(isIdentifierBody(*First) && "invalid identifer");
  const char *Last = First + 1;
  while (Last != End && isIdentifierBody(*Last))
    ++Last;
  return Last;
}

LLVM_NODISCARD static const char *
getIdentifierContinuation(const char *First, const char *const End) {
  if (End - First < 3 || First[0] != '\\' || !isVerticalWhitespace(First[1]))
    return nullptr;

  ++First;
  skipNewline(First, End);
  if (First == End)
    return nullptr;
  return isIdentifierBody(First[0]) ? First : nullptr;
}

Minimizer::IdInfo Minimizer::lexIdentifier(const char *First,
                                           const char *const End) {
  const char *Last = lexRawIdentifier(First, End);
  const char *Next = getIdentifierContinuation(Last, End);
  if (LLVM_LIKELY(!Next))
    return IdInfo{Last, StringRef(First, Last - First)};

  // Slow path, where identifiers are split over lines.
  SmallVector<char, 64> Id(First, Last);
  while (Next) {
    Last = lexRawIdentifier(Next, End);
    Id.append(Next, Last);
    Next = getIdentifierContinuation(Last, End);
  }
  return IdInfo{
      Last,
      SplitIds.try_emplace(StringRef(Id.begin(), Id.size()), 0).first->first()};
}

void Minimizer::printAdjacentMacroArgs(const char *&First,
                                       const char *const End) {
  // Skip over parts of the body.
  const char *Last = First;
  do
    ++Last;
  while (Last != End &&
         (isIdentifierBody(*Last) || *Last == '.' || *Last == ','));
  append(First, Last);
  First = Last;
}

bool Minimizer::printMacroArgs(const char *&First, const char *const End) {
  assert(*First == '(');
  put(*First++);
  for (;;) {
    skipWhitespace(First, End);
    if (First == End)
      return true;

    if (*First == ')') {
      put(*First++);
      return false;
    }

    // This is intentionally fairly liberal.
    if (!(isIdentifierBody(*First) || *First == '.' || *First == ','))
      return true;

    printAdjacentMacroArgs(First, End);
  }
}

/// Looks for an identifier starting from Last.
///
/// Updates "First" to just past the next identifier, if any.  Returns true iff
/// the identifier matches "Id".
bool Minimizer::isNextIdentifier(StringRef Id, const char *&First,
                                 const char *const End) {
  skipWhitespace(First, End);
  if (First == End || !isIdentifierHead(*First))
    return false;

  IdInfo FoundId = lexIdentifier(First, End);
  First = FoundId.Last;
  return FoundId.Name == Id;
}

bool Minimizer::lexAt(const char *&First, const char *const End) {
  // Handle "@import".
  const char *ImportLoc = First++;
  if (!isNextIdentifier("import", First, End)) {
    skipLine(First, End);
    return false;
  }
  makeToken(decl_at_import);
  append("@import ");
  if (printAtImportBody(First, End))
    return reportError(
        ImportLoc, diag::err_dep_source_minimizer_missing_sema_after_at_import);
  skipWhitespace(First, End);
  if (First == End)
    return false;
  if (!isVerticalWhitespace(*First))
    return reportError(
        ImportLoc, diag::err_dep_source_minimizer_unexpected_tokens_at_import);
  skipNewline(First, End);
  return false;
}

bool Minimizer::lexDefine(const char *&First, const char *const End) {
  makeToken(pp_define);
  append("#define ");
  skipWhitespace(First, End);

  if (!isIdentifierHead(*First))
    return reportError(First, diag::err_pp_macro_not_identifier);

  IdInfo Id = lexIdentifier(First, End);
  const char *Last = Id.Last;
  append(Id.Name);
  if (Last == End)
    return false;
  if (*Last == '(') {
    size_t Size = Out.size();
    if (printMacroArgs(Last, End)) {
      // Be robust to bad macro arguments, since they can show up in disabled
      // code.
      Out.resize(Size);
      append("(/* invalid */\n");
      skipLine(Last, End);
      return false;
    }
  }
  skipWhitespace(Last, End);
  if (Last == End)
    return false;
  if (!isVerticalWhitespace(*Last))
    put(' ');
  printDirectiveBody(Last, End);
  First = Last;
  return false;
}

bool Minimizer::lexPragma(const char *&First, const char *const End) {
  // #pragma.
  if (!isNextIdentifier("clang", First, End)) {
    skipLine(First, End);
    return false;
  }

  // #pragma clang.
  if (!isNextIdentifier("module", First, End)) {
    skipLine(First, End);
    return false;
  }

  // #pragma clang module.
  if (!isNextIdentifier("import", First, End)) {
    skipLine(First, End);
    return false;
  }

  // #pragma clang module import.
  makeToken(pp_pragma_import);
  append("#pragma clang module import ");
  printDirectiveBody(First, End);
  return false;
}

bool Minimizer::lexEndif(const char *&First, const char *const End) {
  // Strip out "#else" if it's empty.
  if (top() == pp_else)
    popToken();

  // Strip out "#elif" if they're empty.
  while (top() == pp_elif)
    popToken();

  // If "#if" is empty, strip it and skip the "#endif".
  if (top() == pp_if || top() == pp_ifdef || top() == pp_ifndef) {
    popToken();
    skipLine(First, End);
    return false;
  }

  return lexDefault(pp_endif, "endif", First, End);
}

bool Minimizer::lexDefault(TokenKind Kind, StringRef Directive,
                           const char *&First, const char *const End) {
  makeToken(Kind);
  put('#').append(Directive).put(' ');
  printDirectiveBody(First, End);
  return false;
}

bool Minimizer::lexPPLine(const char *&First, const char *const End) {
  assert(First != End);

  skipWhitespace(First, End);
  assert(First <= End);
  if (First == End)
    return false;

  if (*First != '#' && *First != '@') {
    skipLine(First, End);
    assert(First <= End);
    return false;
  }

  // Handle "@import".
  if (*First == '@')
    return lexAt(First, End);

  // Handle preprocessing directives.
  ++First; // Skip over '#'.
  skipWhitespace(First, End);

  if (First == End)
    return reportError(First, diag::err_pp_expected_eol);

  if (!isIdentifierHead(*First)) {
    skipLine(First, End);
    return false;
  }

  // Figure out the token.
  IdInfo Id = lexIdentifier(First, End);
  First = Id.Last;
  auto Kind = llvm::StringSwitch<TokenKind>(Id.Name)
                  .Case("include", pp_include)
                  .Case("__include_macros", pp___include_macros)
                  .Case("define", pp_define)
                  .Case("undef", pp_undef)
                  .Case("import", pp_import)
                  .Case("include_next", pp_include_next)
                  .Case("if", pp_if)
                  .Case("ifdef", pp_ifdef)
                  .Case("ifndef", pp_ifndef)
                  .Case("elif", pp_elif)
                  .Case("else", pp_else)
                  .Case("endif", pp_endif)
                  .Case("pragma", pp_pragma_import)
                  .Default(pp_none);
  if (Kind == pp_none) {
    skipDirective(Id.Name, First, End);
    return false;
  }

  if (Kind == pp_endif)
    return lexEndif(First, End);

  if (Kind == pp_define)
    return lexDefine(First, End);

  if (Kind == pp_pragma_import)
    return lexPragma(First, End);

  // Everything else.
  return lexDefault(Kind, Id.Name, First, End);
}

bool Minimizer::minimizeImpl(const char *First, const char *const End) {
  while (First != End)
    if (lexPPLine(First, End))
      return true;
  return false;
}

bool Minimizer::minimize() {
  bool Error = minimizeImpl(Input.begin(), Input.end());

  if (!Error) {
    // Add a trailing newline and an EOF on success.
    if (!Out.empty() && Out.back() != '\n')
      Out.push_back('\n');
    makeToken(pp_eof);
  }

  // Null-terminate the output. This way the memory buffer that's passed to
  // Clang will not have to worry about the terminating '\0'.
  Out.push_back(0);
  Out.pop_back();
  return Error;
}

bool clang::minimizeSourceToDependencyDirectives(
    StringRef Input, SmallVectorImpl<char> &Output,
    SmallVectorImpl<Token> &Tokens, DiagnosticsEngine *Diags,
    SourceLocation InputSourceLoc) {
  Output.clear();
  Tokens.clear();
  return Minimizer(Output, Tokens, Input, Diags, InputSourceLoc).minimize();
}
