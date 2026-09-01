// Symbol tables.
//
// Every codepoint/face pairing below was checked against the actual cmaps of the
// bundled KaTeX fonts, so a lookup that succeeds is guaranteed to rasterise.
// Two commands have no glyph anywhere in the font set and are built by the
// layout pass instead: \neq and \notin are drawn as \not applied to = and \in.
#include <algorithm>
#include <string_view>
#include <unordered_map>

#include "tex/tex.h"

namespace sb::tex {
namespace {

using M = std::unordered_map<std::string_view, SymbolDef>;

const M& SymbolTable() {
  static const M table = [] {
    M t;
    const Face R = Face::MainRegular;
    const Face I = Face::MathItalic;
    const Face A = Face::AMS;
    const Atom O = Atom::Ord, B = Atom::Bin, L = Atom::Rel;
    const Atom Op = Atom::Open, Cl = Atom::Close, P = Atom::Punct;

    // ---- lowercase Greek: italic, like TeX ----------------------------------
    t["alpha"] = {0x3B1, I, O};      t["beta"] = {0x3B2, I, O};
    t["gamma"] = {0x3B3, I, O};      t["delta"] = {0x3B4, I, O};
    t["epsilon"] = {0x3F5, I, O};    t["varepsilon"] = {0x3B5, I, O};
    t["zeta"] = {0x3B6, I, O};       t["eta"] = {0x3B7, I, O};
    t["theta"] = {0x3B8, I, O};      t["vartheta"] = {0x3D1, I, O};
    t["iota"] = {0x3B9, I, O};       t["kappa"] = {0x3BA, I, O};
    t["lambda"] = {0x3BB, I, O};     t["mu"] = {0x3BC, I, O};
    t["nu"] = {0x3BD, I, O};         t["xi"] = {0x3BE, I, O};
    t["pi"] = {0x3C0, I, O};         t["varpi"] = {0x3D6, I, O};
    t["rho"] = {0x3C1, I, O};        t["varrho"] = {0x3F1, I, O};
    t["sigma"] = {0x3C3, I, O};      t["varsigma"] = {0x3C2, I, O};
    t["tau"] = {0x3C4, I, O};        t["upsilon"] = {0x3C5, I, O};
    t["phi"] = {0x3D5, I, O};        t["varphi"] = {0x3C6, I, O};
    t["chi"] = {0x3C7, I, O};        t["psi"] = {0x3C8, I, O};
    t["omega"] = {0x3C9, I, O};

    // ---- uppercase Greek: upright, like TeX ---------------------------------
    t["Gamma"] = {0x393, R, O};      t["Delta"] = {0x394, R, O};
    t["Theta"] = {0x398, R, O};      t["Lambda"] = {0x39B, R, O};
    t["Xi"] = {0x39E, R, O};         t["Pi"] = {0x3A0, R, O};
    t["Sigma"] = {0x3A3, R, O};      t["Upsilon"] = {0x3A5, R, O};
    t["Phi"] = {0x3A6, R, O};        t["Psi"] = {0x3A8, R, O};
    t["Omega"] = {0x3A9, R, O};

    // ---- binary operators ---------------------------------------------------
    t["pm"] = {0xB1, R, B};          t["mp"] = {0x2213, R, B};
    t["times"] = {0xD7, R, B};       t["div"] = {0xF7, R, B};
    t["cdot"] = {0x22C5, R, B};      t["ast"] = {0x2217, R, B};
    t["star"] = {0x22C6, R, B};      t["circ"] = {0x2218, R, B};
    t["bullet"] = {0x2219, R, B};    t["oplus"] = {0x2295, R, B};
    t["ominus"] = {0x2296, R, B};    t["otimes"] = {0x2297, R, B};
    t["oslash"] = {0x2298, R, B};    t["odot"] = {0x2299, R, B};
    t["cup"] = {0x222A, R, B};       t["cap"] = {0x2229, R, B};
    t["wedge"] = {0x2227, R, B};     t["land"] = {0x2227, R, B};
    t["vee"] = {0x2228, R, B};       t["lor"] = {0x2228, R, B};
    t["setminus"] = {0x2216, R, B};  t["triangleleft"] = {0x25C3, R, B};
    t["dagger"] = {0x2020, R, B};    t["ddagger"] = {0x2021, R, B};

    // ---- relations ----------------------------------------------------------
    t["leq"] = {0x2264, R, L};       t["le"] = {0x2264, R, L};
    t["geq"] = {0x2265, R, L};       t["ge"] = {0x2265, R, L};
    t["ll"] = {0x226A, R, L};        t["gg"] = {0x226B, R, L};
    t["equiv"] = {0x2261, R, L};     t["sim"] = {0x223C, R, L};
    t["simeq"] = {0x2243, R, L};     t["approx"] = {0x2248, R, L};
    t["cong"] = {0x2245, R, L};      t["propto"] = {0x221D, R, L};
    t["subset"] = {0x2282, R, L};    t["supset"] = {0x2283, R, L};
    t["subseteq"] = {0x2286, R, L};  t["supseteq"] = {0x2287, R, L};
    t["in"] = {0x2208, R, L};        t["ni"] = {0x220B, R, L};
    t["perp"] = {0x22A5, R, L};      t["mid"] = {0x2223, R, L};
    t["parallel"] = {0x2225, R, L};  t["prec"] = {0x227A, R, L};
    t["succ"] = {0x227B, R, L};      t["asymp"] = {0x224D, R, L};
    t["models"] = {0x22A8, A, L};

    // ---- arrows -------------------------------------------------------------
    t["rightarrow"] = {0x2192, R, L};      t["to"] = {0x2192, R, L};
    t["leftarrow"] = {0x2190, R, L};       t["gets"] = {0x2190, R, L};
    t["leftrightarrow"] = {0x2194, R, L};  t["Rightarrow"] = {0x21D2, R, L};
    t["Leftarrow"] = {0x21D0, R, L};       t["Leftrightarrow"] = {0x21D4, R, L};
    t["implies"] = {0x21D2, R, L};         t["iff"] = {0x21D4, R, L};
    t["mapsto"] = {0x21A6, R, L};          t["hookrightarrow"] = {0x21AA, R, L};
    t["uparrow"] = {0x2191, R, L};         t["downarrow"] = {0x2193, R, L};

    // ---- ordinary symbols ---------------------------------------------------
    t["infty"] = {0x221E, R, O};     t["partial"] = {0x2202, R, O};
    t["nabla"] = {0x2207, R, O};     t["forall"] = {0x2200, R, O};
    t["exists"] = {0x2203, R, O};    t["neg"] = {0xAC, R, O};
    t["lnot"] = {0xAC, R, O};        t["emptyset"] = {0x2205, A, O};
    t["varnothing"] = {0x2205, A, O};t["aleph"] = {0x2135, R, O};
    t["hbar"] = {0x210F, R, O};      t["ell"] = {0x2113, R, O};
    t["Re"] = {0x211C, R, O};        t["Im"] = {0x2111, R, O};
    t["wp"] = {0x2118, R, O};        t["prime"] = {0x2032, R, O};
    t["degree"] = {0xB0, R, O};      t["angle"] = {0x2220, A, O};
    t["triangle"] = {0x25B3, A, O};  t["surd"] = {0x221A, R, O};
    t["top"] = {0x22A4, R, O};       t["bot"] = {0x22A5, R, O};
    t["flat"] = {0x266D, R, O};      t["sharp"] = {0x266F, R, O};
    t["ldots"] = {0x2026, R, Atom::Inner};
    t["dots"] = {0x2026, R, Atom::Inner};
    t["cdots"] = {0x22EF, R, Atom::Inner};
    t["vdots"] = {0x22EE, R, Atom::Inner};
    t["ddots"] = {0x22F1, R, Atom::Inner};

    // ---- fences -------------------------------------------------------------
    t["langle"] = {0x27E8, R, Op};   t["rangle"] = {0x27E9, R, Cl};
    t["lceil"] = {0x2308, R, Op};    t["rceil"] = {0x2309, R, Cl};
    t["lfloor"] = {0x230A, R, Op};   t["rfloor"] = {0x230B, R, Cl};
    t["vert"] = {0x2223, R, O};      t["Vert"] = {0x2225, R, O};
    t["|"] = {0x2225, R, O};
    t["{"] = {0x7B, R, Op};          t["}"] = {0x7D, R, Cl};
    t["lbrace"] = {0x7B, R, Op};     t["rbrace"] = {0x7D, R, Cl};
    t["lbrack"] = {0x5B, R, Op};     t["rbrack"] = {0x5D, R, Cl};
    t["colon"] = {0x3A, R, P};

    return t;
  }();
  return table;
}

const std::unordered_map<std::string_view, BigOpDef>& BigOpTable() {
  static const std::unordered_map<std::string_view, BigOpDef> table = {
      {"sum", {0x2211, true}},      {"prod", {0x220F, true}},
      {"coprod", {0x2210, true}},   {"bigcup", {0x22C3, true}},
      {"bigcap", {0x22C2, true}},   {"bigvee", {0x22C1, true}},
      {"bigwedge", {0x22C0, true}}, {"bigoplus", {0x2A01, true}},
      {"bigotimes", {0x2A02, true}},
      // Integrals set their limits as scripts, not above and below.
      {"int", {0x222B, false}},     {"oint", {0x222E, false}},
  };
  return table;
}

const std::unordered_map<std::string_view, SymbolDef>& AccentTable() {
  static const std::unordered_map<std::string_view, SymbolDef> table = {
      {"hat", {0x02C6, Face::MainRegular, Atom::Ord}},
      {"widehat", {0x02C6, Face::MainRegular, Atom::Ord}},
      {"check", {0x02C7, Face::MainRegular, Atom::Ord}},
      {"bar", {0x02C9, Face::MainRegular, Atom::Ord}},
      {"acute", {0x02CA, Face::MainRegular, Atom::Ord}},
      {"grave", {0x02CB, Face::MainRegular, Atom::Ord}},
      {"breve", {0x02D8, Face::MainRegular, Atom::Ord}},
      {"dot", {0x02D9, Face::MainRegular, Atom::Ord}},
      {"tilde", {0x02DC, Face::MainRegular, Atom::Ord}},
      {"widetilde", {0x02DC, Face::MainRegular, Atom::Ord}},
      {"ddot", {0x00A8, Face::MainRegular, Atom::Ord}},
      {"vec", {0x20D7, Face::MainRegular, Atom::Ord}},
  };
  return table;
}

}  // namespace

const SymbolDef* FindSymbol(std::string_view name) {
  const M& t = SymbolTable();
  auto it = t.find(name);
  return (it == t.end()) ? nullptr : &it->second;
}

const BigOpDef* FindBigOp(std::string_view name) {
  const auto& t = BigOpTable();
  auto it = t.find(name);
  return (it == t.end()) ? nullptr : &it->second;
}

const SymbolDef* FindAccent(std::string_view name) {
  const auto& t = AccentTable();
  auto it = t.find(name);
  return (it == t.end()) ? nullptr : &it->second;
}

bool IsFunctionName(std::string_view name, bool* takesLimits) {
  // Upright operator names. The second group takes limits below in display
  // style, the way \lim does.
  static const std::string_view plain[] = {
      "sin", "cos", "tan", "cot", "sec", "csc", "arcsin", "arccos", "arctan",
      "sinh", "cosh", "tanh", "coth", "exp", "log", "ln", "lg", "arg", "deg",
      "dim", "hom", "ker", "det", "gcd", "Pr"};
  static const std::string_view limity[] = {"lim", "limsup", "liminf", "max",
                                            "min", "sup", "inf"};
  for (std::string_view s : plain) {
    if (s == name) { if (takesLimits) *takesLimits = false; return true; }
  }
  for (std::string_view s : limity) {
    if (s == name) { if (takesLimits) *takesLimits = true; return true; }
  }
  return false;
}

int DelimiterCodepoint(std::string_view token) {
  if (token.size() == 1) {
    switch (token[0]) {
      case '(': return 0x28;
      case ')': return 0x29;
      case '[': return 0x5B;
      case ']': return 0x5D;
      case '/': return 0x2F;
      case '|': return 0x2223;
      case '<': return 0x27E8;
      case '>': return 0x27E9;
      case '.': return 0;  // LaTeX's explicit "no delimiter"
      default: break;
    }
  }
  if (const SymbolDef* s = FindSymbol(token)) {
    // Only the fence-ish symbols are usable as \left/\right delimiters.
    switch (s->codepoint) {
      case 0x27E8: case 0x27E9: case 0x2308: case 0x2309:
      case 0x230A: case 0x230B: case 0x2223: case 0x2225:
      case 0x7B:   case 0x7D:   case 0x5B:   case 0x5D:
      case 0x2191: case 0x2193:
        return s->codepoint;
      default: break;
    }
  }
  return -1;  // not a delimiter at all (distinct from 0 = "none")
}

SymbolDef ClassifyCharacter(int cp) {
  // Letters are italic variables; digits and punctuation stay upright. This is
  // exactly TeX's default math alphabet behaviour.
  if ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z')) {
    return {cp, Face::MathItalic, Atom::Ord};
  }
  if (cp >= '0' && cp <= '9') return {cp, Face::MainRegular, Atom::Ord};

  switch (cp) {
    case '+': return {0x2B, Face::MainRegular, Atom::Bin};
    case '-': return {0x2212, Face::MainRegular, Atom::Bin};  // real minus sign
    case '*': return {0x2217, Face::MainRegular, Atom::Bin};
    case '=': return {0x3D, Face::MainRegular, Atom::Rel};
    case '<': return {0x3C, Face::MainRegular, Atom::Rel};
    case '>': return {0x3E, Face::MainRegular, Atom::Rel};
    case '(': return {0x28, Face::MainRegular, Atom::Open};
    case '[': return {0x5B, Face::MainRegular, Atom::Open};
    case ')': return {0x29, Face::MainRegular, Atom::Close};
    case ']': return {0x5D, Face::MainRegular, Atom::Close};
    case ',': return {0x2C, Face::MainRegular, Atom::Punct};
    case ';': return {0x3B, Face::MainRegular, Atom::Punct};
    case ':': return {0x3A, Face::MainRegular, Atom::Rel};
    case '!': return {0x21, Face::MainRegular, Atom::Close};
    case '?': return {0x3F, Face::MainRegular, Atom::Close};
    case '/': return {0x2F, Face::MainRegular, Atom::Ord};
    case '|': return {0x2223, Face::MainRegular, Atom::Ord};
    case '.': return {0x2E, Face::MainRegular, Atom::Ord};
    case '\'': return {0x2032, Face::MainRegular, Atom::Ord};
    default: break;
  }
  return {cp, Face::MainRegular, Atom::Ord};
}

}  // namespace sb::tex
