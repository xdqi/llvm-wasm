//===- ScriptParser.cpp ---------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains a recursive-descendent parser for linker scripts.
// Parsed results are stored to Config and Script global objects.
//
//===----------------------------------------------------------------------===//

#include "ScriptParser.h"
#include "OutputSections.h"
#include "OutputSegment.h"
#include "ScriptLexer.h"
#include "SymbolTable.h"
#include "Symbols.h"
#include "lld/Common/CommonLinkerContext.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SaveAndRestore.h"
#include "llvm/Support/TimeProfiler.h"
#include <cassert>
#include <limits>
#include <vector>

using namespace llvm;
using namespace llvm::support::endian;
using namespace lld;
using namespace lld::wasm;

static StringRef unquote(StringRef s) {
  if (s.starts_with("\""))
    return s.substr(1, s.size() - 2);
  return s;
}

// Some operations only support one non absolute value. Move the
// absolute one to the right hand side for convenience.
static void moveAbsRight(ExprValue &a, ExprValue &b) {
  if (a.sec == nullptr || (a.forceAbsolute && !b.isAbsolute()))
    std::swap(a, b);
  if (!b.isAbsolute())
    error(a.loc + ": at least one side of the expression must be absolute");
}

static ExprValue add(ExprValue a, ExprValue b) {
  moveAbsRight(a, b);
  return {a.sec, a.forceAbsolute, a.getSectionOffset() + b.getValue(), a.loc};
}

static ExprValue sub(ExprValue a, ExprValue b) {
  // The distance between two symbols in sections is absolute.
  if (!a.isAbsolute() && !b.isAbsolute())
    return a.getValue() - b.getValue();
  return {a.sec, false, a.getSectionOffset() - b.getValue(), a.loc};
}

static ExprValue bitAnd(ExprValue a, ExprValue b) {
  moveAbsRight(a, b);
  return {a.sec, a.forceAbsolute,
          (a.getValue() & b.getValue()) - a.getSecAddr(), a.loc};
}

static ExprValue bitOr(ExprValue a, ExprValue b) {
  moveAbsRight(a, b);
  return {a.sec, a.forceAbsolute,
          (a.getValue() | b.getValue()) - a.getSecAddr(), a.loc};
}

uint64_t ExprValue::getValue() const {
  if (sec)
    return alignToPowerOf2(sec->address + sec->getOffset(val),
                           alignment);
  return alignToPowerOf2(val, alignment);
}

uint64_t ExprValue::getSecAddr() const {
  return sec ? sec->address + sec->getOffset(0) : 0;
}

uint64_t ExprValue::getSectionOffset() const {
  // If the alignment is trivial, we don't have to compute the full
  // value to know the offset. This allows this function to succeed in
  // cases where the output section is not yet known.
  if (alignment == 1 && !sec)
    return val;
  return getValue() - getSecAddr();
}

void ScriptParser::readLinkerScript() {
  while (!atEOF()) {
    StringRef tok = next();
    if (tok == ";")
      continue;

    if (tok == "SECTIONS") {
      readSections();
    } else if (tok == "OUTPUT_FORMAT" || tok == "OUTPUT_ARCH" ||
               tok == "ENTRY" || tok == "TARGET") {
      // Ignored: arch/lkl/kernel/vmlinux.lds.S emits OUTPUT_FORMAT("wasm32");
      // wasm-ld already knows the target. Consume "(<token>)".
      expect("(");
      while (!atEOF() && !consume(")"))
        next();
    } else if (SymbolAssignment *cmd = readAssignment(tok)) {
      sectionCommands.push_back(cmd);
    } else {
      setError("unknown directive: " + tok);
    }
  }
}

void ScriptParser::readSections() {
  expect("{");
  SmallVector<SectionCommand *, 0> v;
  while (!errorCount() && !consume("}")) {
    StringRef tok = next();
    if (tok == "OVERLAY") {
      setError("OVERLAY not supported");
      continue;
    }

    if (SectionCommand *cmd = readAssignment(tok))
      v.push_back(cmd);
    else
      v.push_back(readOutputSectionDescription(tok));
  }

  // If DATA_SEGMENT_RELRO_END is absent, for sections after DATA_SEGMENT_ALIGN,
  // the relro fields should be cleared.
/*
  if (!seenRelroEnd)
    for (SectionCommand *cmd : v)
      if (auto *osd = dyn_cast<OutputDesc>(cmd))
        osd->osec.relro = false;
*/
  sectionCommands.insert(sectionCommands.end(), v.begin(), v.end());


  if (atEOF() || !consume("INSERT")) {
    hasSectionsCommand = true;
    return;
  }

  setError("INSERT BEFORE/AFTER not supported");
}

static int precedence(StringRef op) {
  return StringSwitch<int>(op)
      .Cases("*", "/", "%", 10)
      .Cases("+", "-", 9)
      .Cases("<<", ">>", 8)
      .Cases("<", "<=", ">", ">=", 7)
      .Cases("==", "!=", 6)
      .Case("&", 5)
      .Case("|", 4)
      .Case("&&", 3)
      .Case("||", 2)
      .Case("?", 1)
      .Default(-1);
}

StringMatcher ScriptParser::readFilePatterns() {
  StringMatcher Matcher;

  while (!errorCount() && !consume(")"))
    Matcher.addPattern(SingleStringMatcher(next()));
  return Matcher;
}

SortSectionPolicy ScriptParser::peekSortKind() {
  return StringSwitch<SortSectionPolicy>(peek())
      .Cases("SORT", "SORT_BY_NAME", SortSectionPolicy::Name)
      .Case("SORT_BY_ALIGNMENT", SortSectionPolicy::Alignment)
      .Case("SORT_BY_INIT_PRIORITY", SortSectionPolicy::Priority)
      .Case("SORT_NONE", SortSectionPolicy::None)
      .Default(SortSectionPolicy::Default);
}

SortSectionPolicy ScriptParser::readSortKind() {
  SortSectionPolicy ret = peekSortKind();
  if (ret != SortSectionPolicy::Default)
    skip();
  return ret;
}

// Reads SECTIONS command contents in the following form:
//
// <contents> ::= <elem>*
// <elem>     ::= <exclude>? <glob-pattern>
// <exclude>  ::= "EXCLUDE_FILE" "(" <glob-pattern>+ ")"
//
// For example,
//
// *(.foo EXCLUDE_FILE (a.o) .bar EXCLUDE_FILE (b.o) .baz)
//
// is parsed as ".foo", ".bar" with "a.o", and ".baz" with "b.o".
// The semantics of that is section .foo in any file, section .bar in
// any file but a.o, and section .baz in any file but b.o.
SmallVector<SectionPattern, 0> ScriptParser::readInputSectionsList() {
  SmallVector<SectionPattern, 0> ret;
  while (!errorCount() && peek() != ")") {
    StringMatcher excludeFilePat;
    if (consume("EXCLUDE_FILE")) {
      expect("(");
      excludeFilePat = readFilePatterns();
    }

    StringMatcher SectionMatcher;
    // Break if the next token is ), EXCLUDE_FILE, or SORT*.
    while (!errorCount() && peek() != ")" && peek() != "EXCLUDE_FILE" &&
           peekSortKind() == SortSectionPolicy::Default)
      SectionMatcher.addPattern(unquote(next()));

    if (!SectionMatcher.empty())
      ret.push_back({std::move(excludeFilePat), std::move(SectionMatcher)});
    else if (excludeFilePat.empty())
      break;
    else
      setError("section pattern is expected");
  }
  return ret;
}

// Reads contents of "SECTIONS" directive. That directive contains a
// list of glob patterns for input sections. The grammar is as follows.
//
// <patterns> ::= <section-list>
//              | <sort> "(" <section-list> ")"
//              | <sort> "(" <sort> "(" <section-list> ")" ")"
//
// <sort>     ::= "SORT" | "SORT_BY_NAME" | "SORT_BY_ALIGNMENT"
//              | "SORT_BY_INIT_PRIORITY" | "SORT_NONE"
//
// <section-list> is parsed by readInputSectionsList().
InputSectionDescription *
ScriptParser::readInputSectionRules(StringRef filePattern, uint64_t withFlags,
                                    uint64_t withoutFlags) {
  auto *cmd =
      make<InputSectionDescription>(filePattern, withFlags, withoutFlags);
  expect("(");

  while (!errorCount() && !consume(")")) {
    SortSectionPolicy outer = readSortKind();
    SortSectionPolicy inner = SortSectionPolicy::Default;
    SmallVector<SectionPattern, 0> v;
    if (outer != SortSectionPolicy::Default) {
      expect("(");
      inner = readSortKind();
      if (inner != SortSectionPolicy::Default) {
        expect("(");
        v = readInputSectionsList();
        expect(")");
      } else {
        v = readInputSectionsList();
      }
      expect(")");
    } else {
      v = readInputSectionsList();
    }

    for (SectionPattern &pat : v) {
      pat.sortInner = inner;
      pat.sortOuter = outer;
    }

    std::move(v.begin(), v.end(), std::back_inserter(cmd->sectionPatterns));
  }
  return cmd;
}

InputSectionDescription *
ScriptParser::readInputSectionDescription(StringRef tok) {
  // Input section wildcard can be surrounded by KEEP.
  // https://sourceware.org/binutils/docs/ld/Input-Section-Keep.html#Input-Section-Keep
  uint64_t withFlags = 0;
  uint64_t withoutFlags = 0;
  if (tok == "KEEP") {
    expect("(");
    if (consume("INPUT_SECTION_FLAGS"))
      setError("INPUT_SECTION_FLAGS not supported");
    InputSectionDescription *cmd =
        readInputSectionRules(next(), withFlags, withoutFlags);
    expect(")");
    keptSections.push_back(cmd);
    return cmd;
  }
  if (tok == "INPUT_SECTION_FLAGS") {
    setError("INPUT_SECTION_FLAGS not supported");
    tok = next();
  }
  return readInputSectionRules(tok, withFlags, withoutFlags);
}

void ScriptParser::readSort() {
  expect("(");
  expect("CONSTRUCTORS");
  expect(")");
}

Expr ScriptParser::readAssert() {
  expect("(");
  Expr e = readExpr();
  expect(",");
  StringRef msg = unquote(next());
  expect(")");

  return [=] {
    if (!e().getValue())
      error(msg);
    return dot;
  };
}

/*
#define ECase(X)                                                               \
  { #X, X }
constexpr std::pair<const char *, unsigned> typeMap[] = {
    ECase(SHT_PROGBITS),   ECase(SHT_NOTE),       ECase(SHT_NOBITS),
    ECase(SHT_INIT_ARRAY), ECase(SHT_FINI_ARRAY), ECase(SHT_PREINIT_ARRAY),
};
#undef ECase
*/
// Tries to read the special directive for an output section definition which
// can be one of following: "(NOLOAD)", "(COPY)", "(INFO)", "(OVERLAY)", and
// "(TYPE=<value>)".
// Tok1 and Tok2 are next 2 tokens peeked. See comment for
// readSectionAddressType below.
bool ScriptParser::readSectionDirective(SectionBase *osec, StringRef tok1, StringRef tok2) {
  if (tok1 != "(")
    return false;
  if (tok2 != "NOLOAD" && tok2 != "COPY" && tok2 != "INFO" &&
      tok2 != "OVERLAY" && tok2 != "TYPE")
    return false;

  expect("(");
  setError("section directive " + tok2 + " currently not supported");
  if (consume("TYPE"))
    { expect("="); readExpr(); }
  else
    skip();

  // cmd = osec->outputSection applies below
/*  if (consume("NOLOAD")) {
    cmd->type = SHT_NOBITS;
    cmd->typeIsSet = true;
  } else if (consume("TYPE")) {
    expect("=");
    StringRef value = peek();
    auto it = llvm::find_if(typeMap, [=](auto e) { return e.first == value; });
    if (it != std::end(typeMap)) {
      // The value is a recognized literal SHT_*.
      cmd->type = it->second;
      skip();
    } else if (value.starts_with("SHT_")) {
      setError("unknown section type " + value);
    } else {
      // Otherwise, read an expression.
      cmd->type = readExpr()().getValue();
    }
    cmd->typeIsSet = true;
  } else {
    skip(); // This is "COPY", "INFO" or "OVERLAY".
    cmd->nonAlloc = true;
  }
*/
  expect(")");
  return true;
}

// Reads an expression and/or the special directive for an output
// section definition. Directive is one of following: "(NOLOAD)",
// "(COPY)", "(INFO)" or "(OVERLAY)".
//
// An output section name can be followed by an address expression
// and/or directive. This grammar is not LL(1) because "(" can be
// interpreted as either the beginning of some expression or beginning
// of directive.
//
// https://sourceware.org/binutils/docs/ld/Output-Section-Address.html
// https://sourceware.org/binutils/docs/ld/Output-Section-Type.html
void ScriptParser::readSectionAddressType(SectionBase *osec) {
  // Temporarily set inExpr to support TYPE=<value> without spaces.
  bool saved = std::exchange(inExpr, true);
  bool isDirective = readSectionDirective(osec, peek(), peek2());
  inExpr = saved;
  if (isDirective)
    return;

  osec->address = readExpr()().getValue();
  setError("setting address for " + osec->name + " to " + Twine(osec->address));
  if (peek() == "(" && !readSectionDirective(osec, "(", peek2()))
    setError("unknown section directive: " + peek2());
}

static Expr checkAlignment(Expr e, std::string &loc) {
  return [=] {
    uint64_t alignment = std::max((uint64_t)1, e().getValue());
    if (!isPowerOf2_64(alignment)) {
      error(loc + ": alignment must be power of 2");
      return (uint64_t)1; // Return a dummy value.
    }
    return alignment;
  };
}

OutputDesc *ScriptParser::readOutputSectionDescription(StringRef outSec) {
  OutputDesc *cmd = createOutputSection(outSec, getCurrentLocation());
  SectionBase *osec = &cmd->osec;
  // Maybe relro. Will reset to false if DATA_SEGMENT_RELRO_END is absent.
  //osec->relro = seenDataAlign && !seenRelroEnd;

  //size_t symbolsReferenced = referencedSymbols.size();

  if (peek() != ":") {
    readSectionAddressType(osec);
  }
  expect(":");

  std::string location = getCurrentLocation();
  if (consume("AT"))
    //cmd->lmaExpr = readParenExpr();
    cmd->osec.address = readParenExpr()().getValue();
  if (consume("ALIGN"))
    //cmd->alignExpr = checkAlignment(readParenExpr(), location);
    //{ uint64_t align = checkAlignment(readParenExpr(), location)();
    //  cmd->osec.address = (cmd->osec->address + (align - 1U)) & ~(align - 1U); }
    setError("setting ALIGN on a section unsupported, align the dot instead");
  if (consume("SUBALIGN"))
    error("SUBALIGN unsupported");
    //osec->subalignExpr = checkAlignment(readParenExpr(), location);

  // Parse constraints.
  if (consume("ONLY_IF_RO"))
    setError("constraints like ONLY_IF_RO unsuported");
    //osec->constraint = ConstraintKind::ReadOnly;
  if (consume("ONLY_IF_RW"))
    setError("constraints like ONLY_IF_RW unsuported");
    //osec->constraint = ConstraintKind::ReadWrite;
  expect("{");

  while (!errorCount() && !consume("}")) {
    StringRef tok = next();
    if (tok == ";") {
      // Empty commands are allowed. Do nothing here.
    } else if (SymbolAssignment *assign = readAssignment(tok)) {
      osec->commands.push_back(assign);
    } else if (ByteCommand *data = readByteCommand(tok)) {
      osec->commands.push_back(data);
    } else if (tok == "CONSTRUCTORS") {
      // CONSTRUCTORS is a keyword to make the linker recognize C++ ctors/dtors
      // by name. This is for very old file formats such as ECOFF/XCOFF.
      // For ELF, we should ignore.
    } else if (tok == "FILL") {
      // We handle the FILL command as an alias for =fillexp section attribute,
      // which is different from what GNU linkers do.
      // https://sourceware.org/binutils/docs/ld/Output-Section-Data.html
      if (peek() != "(")
        setError("( expected, but got " + peek());
      setError("FILL unsupported"); //osec->filler = readFill();
    } else if (tok == "SORT") {
      readSort();
    } else if (tok == "INCLUDE") {
      setError("INCLUDE not supported");
    } else if (tok == "(" || tok == ")") {
      setError("expected filename pattern");
    } else if (peek() == "(") {
      osec->commands.push_back(readInputSectionDescription(tok));
    } else {
      // We have a file name and no input sections description. It is not a
      // commonly used syntax, but still acceptable. In that case, all sections
      // from the file will be included.
      // FIXME: GNU ld permits INPUT_SECTION_FLAGS to be used here. We do not
      // handle this case here as it will already have been matched by the
      // case above.
      auto *isd = make<InputSectionDescription>(tok);
      isd->sectionPatterns.push_back({{}, StringMatcher("*")});
      osec->commands.push_back(isd);
    }
  }

  if (consume(">"))
    setError("using > not supported");
    //osec->memoryRegionName = std::string(next());

  if (consume("AT")) {
    setError("using AT > not supported");
    expect(">");
    //osec->lmaRegionName = std::string(next());
  }

  //if (osec->lmaExpr && !osec->lmaRegionName.empty())
  //  error("section can't have both LMA and a load region");

  //osec->phdrs = readOutputSectionPhdrs();

  if (peek() == "=" || peek().starts_with("=")) {
    inExpr = true;
    consume("=");
    setError("filler unsupported");
    //osec->filler = readFill();
    inExpr = false;
  }

  // Consume optional comma following output section command.
  consume(",");

  //if (referencedSymbols.size() > symbolsReferenced)
  //  osec->expressionsUseSymbols = true;
  return cmd;
}

// Reads a `=<fillexp>` expression and returns its value as a big-endian number.
// https://sourceware.org/binutils/docs/ld/Output-Section-Fill.html
// We do not support using symbols in such expressions.
//
// When reading a hexstring, ld.bfd handles it as a blob of arbitrary
// size, while ld.gold always handles it as a 32-bit big-endian number.
// We are compatible with ld.gold because it's easier to implement.
// Also, we require that expressions with operators must be wrapped into
// round brackets. We did it to resolve the ambiguity when parsing scripts like:
// SECTIONS { .foo : { ... } =120+3 /DISCARD/ : { ... } }
std::array<uint8_t, 4> ScriptParser::readFill() {
  uint64_t value = readPrimary()().val;
  if (value > UINT32_MAX)
    setError("filler expression result does not fit 32-bit: 0x" +
             Twine::utohexstr(value));

  std::array<uint8_t, 4> buf;
  write32be(buf.data(), (uint32_t)value);
  return buf;
}

SymbolAssignment *ScriptParser::readProvideHidden(bool provide, bool hidden) {
  expect("(");
  StringRef name = next(), eq = peek();
  if (eq != "=") {
    setError("= expected, but got " + next());
    while (!atEOF() && next() != ")")
      ;
    return nullptr;
  }
  SymbolAssignment *cmd = readSymbolAssignment(name);
  cmd->provide = provide;
  cmd->hidden = hidden;
  expect(")");
  return cmd;
}

SymbolAssignment *ScriptParser::readAssignment(StringRef tok) {
  // Assert expression returns Dot, so this is equal to ".=."
  if (tok == "ASSERT")
    return make<SymbolAssignment>(".", readAssert(), getCurrentLocation());

  size_t oldPos = pos;
  SymbolAssignment *cmd = nullptr;
  const StringRef op = peek();
  if (op.starts_with("=")) {
    // Support = followed by an expression without whitespace.
    SaveAndRestore saved(inExpr, true);
    cmd = readSymbolAssignment(tok);
  } else if ((op.size() == 2 && op[1] == '=' && strchr("*/+-&|", op[0])) ||
             op == "<<=" || op == ">>=") {
    cmd = readSymbolAssignment(tok);
  } else if (tok == "PROVIDE") {
    SaveAndRestore saved(inExpr, true);
    cmd = readProvideHidden(true, false);
  } else if (tok == "HIDDEN") {
    SaveAndRestore saved(inExpr, true);
    cmd = readProvideHidden(false, true);
  } else if (tok == "PROVIDE_HIDDEN") {
    SaveAndRestore saved(inExpr, true);
    cmd = readProvideHidden(true, true);
  }

  if (cmd) {
    cmd->commandString =
        tok.str() + " " +
        llvm::join(tokens.begin() + oldPos, tokens.begin() + pos, " ");
    expect(";");
  }
  return cmd;
}

SymbolAssignment *ScriptParser::readSymbolAssignment(StringRef name) {
  name = unquote(name);
  StringRef op = next();
  assert(op == "=" || op == "*=" || op == "/=" || op == "+=" || op == "-=" ||
         op == "&=" || op == "|=" || op == "<<=" || op == ">>=");
  // Note: GNU ld does not support %= or ^=.
  Expr e = readExpr();
  if (op != "=") {
    std::string loc = getCurrentLocation();
    e = [=, c = op[0]]() -> ExprValue {
      ExprValue lhs = getSymbolValue(name, loc);
      switch (c) {
      case '*':
        return lhs.getValue() * e().getValue();
      case '/':
        if (uint64_t rv = e().getValue())
          return lhs.getValue() / rv;
        error(loc + ": division by zero");
        return 0;
      case '+':
        return add(lhs, e());
      case '-':
        return sub(lhs, e());
      case '<':
        return lhs.getValue() << e().getValue();
      case '>':
        return lhs.getValue() >> e().getValue();
      case '&':
        return lhs.getValue() & e().getValue();
      case '|':
        return lhs.getValue() | e().getValue();
      default:
        llvm_unreachable("");
      }
    };
  }
  return make<SymbolAssignment>(name, e, getCurrentLocation());
}

// This is an operator-precedence parser to parse a linker
// script expression.
Expr ScriptParser::readExpr() {
  // Our lexer is context-aware. Set the in-expression bit so that
  // they apply different tokenization rules.
  bool orig = inExpr;
  inExpr = true;
  Expr e = readExpr1(readPrimary(), 0);
  inExpr = orig;
  return e;
}

Expr ScriptParser::combine(StringRef op, Expr l, Expr r) {
  if (op == "+")
    return [=] { return add(l(), r()); };
  if (op == "-")
    return [=] { return sub(l(), r()); };
  if (op == "*")
    return [=] { return l().getValue() * r().getValue(); };
  if (op == "/") {
    std::string loc = getCurrentLocation();
    return [=]() -> uint64_t {
      if (uint64_t rv = r().getValue())
        return l().getValue() / rv;
      error(loc + ": division by zero");
      return 0;
    };
  }
  if (op == "%") {
    std::string loc = getCurrentLocation();
    return [=]() -> uint64_t {
      if (uint64_t rv = r().getValue())
        return l().getValue() % rv;
      error(loc + ": modulo by zero");
      return 0;
    };
  }
  if (op == "<<")
    return [=] { return l().getValue() << r().getValue(); };
  if (op == ">>")
    return [=] { return l().getValue() >> r().getValue(); };
  if (op == "<")
    return [=] { return l().getValue() < r().getValue(); };
  if (op == ">")
    return [=] { return l().getValue() > r().getValue(); };
  if (op == ">=")
    return [=] { return l().getValue() >= r().getValue(); };
  if (op == "<=")
    return [=] { return l().getValue() <= r().getValue(); };
  if (op == "==")
    return [=] { return l().getValue() == r().getValue(); };
  if (op == "!=")
    return [=] { return l().getValue() != r().getValue(); };
  if (op == "||")
    return [=] { return l().getValue() || r().getValue(); };
  if (op == "&&")
    return [=] { return l().getValue() && r().getValue(); };
  if (op == "&")
    return [=] { return bitAnd(l(), r()); };
  if (op == "|")
    return [=] { return bitOr(l(), r()); };
  llvm_unreachable("invalid operator");
}

// This is a part of the operator-precedence parser. This function
// assumes that the remaining token stream starts with an operator.
Expr ScriptParser::readExpr1(Expr lhs, int minPrec) {
  while (!atEOF() && !errorCount()) {
    // Read an operator and an expression.
    StringRef op1 = peek();
    if (precedence(op1) < minPrec)
      break;
    if (consume("?"))
      return readTernary(lhs);
    skip();
    Expr rhs = readPrimary();

    // Evaluate the remaining part of the expression first if the
    // next operator has greater precedence than the previous one.
    // For example, if we have read "+" and "3", and if the next
    // operator is "*", then we'll evaluate 3 * ... part first.
    while (!atEOF()) {
      StringRef op2 = peek();
      if (precedence(op2) <= precedence(op1))
        break;
      rhs = readExpr1(rhs, precedence(op2));
    }

    lhs = combine(op1, lhs, rhs);
  }
  return lhs;
}

Expr ScriptParser::getPageSize() {
  return [] { return 0xFFFF; }; // Wasm page size is 65k.
}

Expr ScriptParser::readConstant() {
  StringRef s = readParenLiteral();
  if (s == "COMMONPAGESIZE")
    return getPageSize();
  if (s == "MAXPAGESIZE")
    return getPageSize();
  setError("unknown constant: " + s);
  return [] { return 0; };
}

// Parses Tok as an integer. It recognizes hexadecimal (prefixed with
// "0x" or suffixed with "H") and decimal numbers. Decimal numbers may
// have "K" (Ki) or "M" (Mi) suffixes.
static std::optional<uint64_t> parseInt(StringRef tok) {
  // Hexadecimal
  uint64_t val;
  if (tok.starts_with_insensitive("0x")) {
    if (!to_integer(tok.substr(2), val, 16))
      return std::nullopt;
    return val;
  }
  if (tok.ends_with_insensitive("H")) {
    if (!to_integer(tok.drop_back(), val, 16))
      return std::nullopt;
    return val;
  }

  // Decimal
  if (tok.ends_with_insensitive("K")) {
    if (!to_integer(tok.drop_back(), val, 10))
      return std::nullopt;
    return val * 1024;
  }
  if (tok.ends_with_insensitive("M")) {
    if (!to_integer(tok.drop_back(), val, 10))
      return std::nullopt;
    return val * 1024 * 1024;
  }
  if (!to_integer(tok, val, 10))
    return std::nullopt;
  return val;
}

ByteCommand *ScriptParser::readByteCommand(StringRef tok) {
  int size = StringSwitch<int>(tok)
                 .Case("BYTE", 1)
                 .Case("SHORT", 2)
                 .Case("LONG", 4)
                 .Case("QUAD", 8)
                 .Default(-1);
  if (size == -1)
    return nullptr;

  size_t oldPos = pos;
  Expr e = readParenExpr();
  std::string commandString =
      tok.str() + " " +
      llvm::join(tokens.begin() + oldPos, tokens.begin() + pos, " ");
  return make<ByteCommand>(e, size, commandString);
}

StringRef ScriptParser::readParenLiteral() {
  expect("(");
  bool orig = inExpr;
  inExpr = false;
  StringRef tok = next();
  inExpr = orig;
  expect(")");
  return tok;
}

static void checkIfExists(const SectionBase &osec, StringRef location) {
  if (osec.location.empty())
    error(location + ": undefined section " + osec.name);
}

static bool isValidSymbolName(StringRef s) {
  auto valid = [](char c) {
    return isAlnum(c) || c == '$' || c == '.' || c == '_';
  };
  return !s.empty() && !isDigit(s[0]) && llvm::all_of(s, valid);
}

Expr ScriptParser::readPrimary() {
  if (peek() == "(")
    return readParenExpr();

  if (consume("~")) {
    Expr e = readPrimary();
    return [=] { return ~e().getValue(); };
  }
  if (consume("!")) {
    Expr e = readPrimary();
    return [=] { return !e().getValue(); };
  }
  if (consume("-")) {
    Expr e = readPrimary();
    return [=] { return -e().getValue(); };
  }

  StringRef tok = next();
  std::string location = getCurrentLocation();

  // Built-in functions are parsed here.
  // https://sourceware.org/binutils/docs/ld/Builtin-Functions.html.
  if (tok == "ABSOLUTE") {
    Expr inner = readParenExpr();
    return [=] {
      ExprValue i = inner();
      i.forceAbsolute = true;
      return i;
    };
  }
  if (tok == "ADDR") {
    StringRef name = readParenLiteral();
    SectionBase *osec = &getOrCreateOutputSection(name)->osec;
    //osec->usedInExpression = true;
    return [=]() -> ExprValue {
      checkIfExists(*osec, location);
      return {osec, false, 0, location};
    };
  }
  if (tok == "ALIGN") {
    expect("(");
    Expr e = readExpr();
    if (consume(")")) {
      e = checkAlignment(e, location);
      return [=] { return alignToPowerOf2(dot, e().getValue()); };
    }
    expect(",");
    Expr e2 = checkAlignment(readExpr(), location);
    expect(")");
    return [=] {
      ExprValue v = e();
      v.alignment = e2().getValue();
      return v;
    };
  }
  if (tok == "ALIGNOF") {
    setError("ALIGNOF unsupported");
    StringRef name = readParenLiteral();
    SectionBase *osec = &getOrCreateOutputSection(name)->osec;
    return [=] {
      checkIfExists(*osec, location);
      return 0;//osec->addralign;
    };
  }
  if (tok == "ASSERT")
    return readAssert();
  if (tok == "CONSTANT")
    return readConstant();
  if (tok == "DATA_SEGMENT_ALIGN") {
    expect("(");
    Expr e = readExpr();
    expect(",");
    readExpr();
    expect(")");
    seenDataAlign = true;
    return [=] {
      uint64_t align = std::max(uint64_t(1), e().getValue());
      return (dot + align - 1) & -align;
    };
  }
  if (tok == "DATA_SEGMENT_END") {
    expect("(");
    expect(".");
    expect(")");
    return [=] { return dot; }; // = added
  }
  if (tok == "DATA_SEGMENT_RELRO_END") {
    setError("unsupported DATA_SEGMENT_RELRO_END");

    // GNU linkers implements more complicated logic to handle
    // DATA_SEGMENT_RELRO_END. We instead ignore the arguments and
    // just align to the next page boundary for simplicity.
    expect("(");
    readExpr();
    expect(",");
    readExpr();
    expect(")");
    seenRelroEnd = true;
    Expr e = getPageSize();
    return [=] { return alignToPowerOf2(dot, e().getValue()); };
  }
  if (tok == "DEFINED") {
    StringRef name = unquote(readParenLiteral());
    return [=] {
      Symbol *b = symtab->find(name);
      return (b && b->isDefined()) ? 1 : 0;
    };
  }
  if (tok == "LENGTH") {
    setError("LENGTH command not supported (no memory region support)");
    return 0;
  }
  if (tok == "LOADADDR") {
    setError("LOADADDR unsuppported");
    /*
    StringRef name = readParenLiteral();
    OutputSection *osec = &getOrCreateOutputSection(name)->osec;
    osec->usedInExpression = true;
    return [=] {
      checkIfExists(*osec, location);
      return osec->getLMA();
    };
    */
  }
  if (tok == "LOG2CEIL") {
    expect("(");
    Expr a = readExpr();
    expect(")");
    return [=] {
      // LOG2CEIL(0) is defined to be 0.
      return llvm::Log2_64_Ceil(std::max(a().getValue(), UINT64_C(1)));
    };
  }
  if (tok == "MAX" || tok == "MIN") {
    expect("(");
    Expr a = readExpr();
    expect(",");
    Expr b = readExpr();
    expect(")");
    if (tok == "MIN")
      return [=] { return std::min(a().getValue(), b().getValue()); };
    return [=] { return std::max(a().getValue(), b().getValue()); };
  }
  if (tok == "ORIGIN") {
    setError("ORIGIN command not supported (no memory region support)");
    return 0;
  }
  if (tok == "SEGMENT_START") {
    expect("(");
    skip();
    expect(",");
    Expr e = readExpr();
    expect(")");
    return [=] { return e(); };
  }
  if (tok == "SIZEOF") {
    setError("SIZEOF unsupported");
    //StringRef name = readParenLiteral();
    //SectionBase *cmd = &getOrCreateOutputSection(name)->osec;
    // Linker script does not create an output section if its content is empty.
    // We want to allow SIZEOF(.foo) where .foo is a section which happened to
    // be empty.
    return [=] { return 0;/*cmd->size;*/ };
  }
  if (tok == "SIZEOF_HEADERS")
    return [=] { return /*elf::getHeaderSize();*/ 0; };

  // Tok is the dot.
  if (tok == ".")
    return [=] { return getSymbolValue(tok, location); };

  // Tok is a literal number.
  if (std::optional<uint64_t> val = parseInt(tok))
    return [=] { return *val; };

  // Tok is a symbol name.
  if (tok.starts_with("\""))
    tok = unquote(tok);
  else if (!isValidSymbolName(tok))
    setError("malformed number: " + tok);
  //referencedSymbols.push_back(tok);
  return [=] { return getSymbolValue(tok, location); };
}

Expr ScriptParser::readTernary(Expr cond) {
  Expr l = readExpr();
  expect(":");
  Expr r = readExpr();
  return [=] { return cond().getValue() ? l() : r(); };
}

Expr ScriptParser::readParenExpr() {
  expect("(");
  Expr e = readExpr();
  expect(")");
  return e;
}

OutputDesc *ScriptParser::createOutputSection(StringRef name,
                                              StringRef location) {
  OutputDesc *&secRef = nameToOutputSection[CachedHashStringRef(name)];
  OutputDesc *sec;
  if (secRef && secRef->osec.location.empty()) {
    // There was a forward reference.
    sec = secRef;
  } else {
    sec = make<OutputDesc>(name);
    if (!secRef)
      secRef = sec;
  }
  sec->osec.location = std::string(location);
  return sec;
}

OutputDesc *ScriptParser::getOrCreateOutputSection(StringRef name) {
  OutputDesc *&cmdRef = nameToOutputSection[CachedHashStringRef(name)];
  if (!cmdRef)
    cmdRef = make<OutputDesc>(name);
  return cmdRef;
}

ExprValue ScriptParser::getSymbolValue(StringRef name, const Twine &loc) {
  if (name == ".") {
    //if (state)
      // return {state->outSec, false, dot - state->outSec->addr, loc};
    return {nullptr, false,  dot, loc};
    //error(loc + ": unable to get location counter value");
    //return 0;
  }

  if (Symbol *sym = symtab->find(name)) {
    if (auto *ds = dyn_cast<DefinedData>(sym)) {
      // A bit of a hack to support aliases outside of SECTIONS.
      // This only works if the evaluation happpens after placement into the output.
      uint64_t offset = ds->segment && ds->segment->outputSeg ? ds->segment->outputSeg->startVA + ds->segment->outputSegmentOffset : ds->value;
      ExprValue v{nullptr, false, offset, loc};
      // Retain the original st_type, so that the alias will get the same
      // behavior in relocation processing. Any operation will reset st_type to
      // STT_NOTYPE.
      // v.type = ds->type;
      return v;
    }
    //if (isa<SharedSymbol>(sym))
    //  if (!errorOnMissingSection)
    //    return {nullptr, false, 0, loc};
  }

  error(loc + ": symbol not found: " + name);
  return 0;
}
