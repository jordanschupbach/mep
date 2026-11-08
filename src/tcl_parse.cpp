// Tcl's parser: see tcl_parse.h.
//
// A port of Tcl 8.6.16's generic/tclParse.c, generic/tclCompExpr.c
// (ParseExpr, ParseLexeme), generic/tclStrToD.c (TclParseNumber's scanner)
// and generic/tclUtil.c (TclFindElement), which carry this notice:
//
// This software is copyrighted by the Regents of the University of
// California, Sun Microsystems, Inc., Scriptics Corporation, ActiveState
// Corporation and other parties.  The following terms apply to all files
// associated with the software unless explicitly disclaimed in
// individual files.
//
// The authors hereby grant permission to use, copy, modify, distribute,
// and license this software and its documentation for any purpose, provided
// that existing copyright notices are retained in all copies and that this
// notice is included verbatim in any distributions. No written agreement,
// license, or royalty fee is required for any of the authorized uses.
// Modifications to this software may be copyrighted by their authors
// and need not follow the licensing terms described here, provided that
// the new terms are clearly indicated on the first page of each file where
// they apply.
//
// IN NO EVENT SHALL THE AUTHORS OR DISTRIBUTORS BE LIABLE TO ANY PARTY
// FOR DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES
// ARISING OUT OF THE USE OF THIS SOFTWARE, ITS DOCUMENTATION, OR ANY
// DERIVATIVES THEREOF, EVEN IF THE AUTHORS HAVE BEEN ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// THE AUTHORS AND DISTRIBUTORS SPECIFICALLY DISCLAIM ANY WARRANTIES,
// INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE, AND NON-INFRINGEMENT.  THIS SOFTWARE
// IS PROVIDED ON AN "AS IS" BASIS, AND THE AUTHORS AND DISTRIBUTORS HAVE
// NO OBLIGATION TO PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR
// MODIFICATIONS.
//
// GOVERNMENT USE: If you are acquiring this software on behalf of the
// U.S. government, the Government shall have only "Restricted Rights"
// in the software and related documentation as defined in the Federal
// Acquisition Regulations (FARs) in Clause 52.227.19 (c) (2).  If you
// are acquiring the software on behalf of the Department of Defense, the
// software shall be classified as "Commercial Computer Software" and the
// Government shall have only "Restricted Rights" as defined in Clause
// 252.227-7014 (b) (3) of DFARs.  Notwithstanding the foregoing, the
// authors grant the U.S. Government and others acting in its behalf
// permission to use and distribute the software in accordance with the
// terms specified in this license.
#include "tcl_parse.h"

#include <cctype>
#include <cstdio>

namespace tclparse {

namespace {

enum : unsigned {
    TYPE_NORMAL = 0,
    TYPE_SPACE = 0x1,
    TYPE_COMMAND_END = 0x2,
    TYPE_SUBS = 0x4,
    TYPE_QUOTE = 0x8,
    TYPE_CLOSE_PAREN = 0x10,
    TYPE_CLOSE_BRACK = 0x20,
    TYPE_BRACE = 0x40,
};

unsigned CharType(char c) {
    switch (c) {
    case '\0': case '$': case '[': case '\\': return TYPE_SUBS;
    case '\t': case '\v': case '\f': case '\r': case ' ': return TYPE_SPACE;
    case '\n': case ';': return TYPE_COMMAND_END;
    case '"': return TYPE_QUOTE;
    case ')': return TYPE_CLOSE_PAREN;
    case ']': return TYPE_CLOSE_BRACK;
    case '{': case '}': return TYPE_BRACE;
    default: return TYPE_NORMAL;
    }
}

// The state of one Tcl_Parse: the text, where it ends, the token array.
struct Parse {
    std::string_view t;
    int end = 0;
    std::vector<Token> *tokens = nullptr;
    Error *err = nullptr;
    int term = 0;
    bool incomplete = false;
    int depth = 0;

    char at(int i) const { return i >= 0 && i < end ? t[static_cast<size_t>(i)] : '\0'; }
    int num() const { return static_cast<int>(tokens->size()); }
    Token &tok(int i) { return (*tokens)[static_cast<size_t>(i)]; }
    void fail(const char *msg) {
        if (err->failed) return;
        err->failed = true;
        err->message = msg;
    }
};

constexpr int kMaxDepth = 1000;  // nested [ ] before we give up rather than overflow the stack

bool ParseCommandImpl(Parse &outer, int start, int numBytes, bool nested, Command *cmd);

int ParseWhiteSpace(const Parse &p, int src, int numBytes, bool *incomplete, unsigned *typePtr) {
    unsigned type = TYPE_NORMAL;
    int q = src;
    while (true) {
        while (numBytes && ((type = CharType(p.at(q))) & TYPE_SPACE)) {
            numBytes--;
            q++;
        }
        if (numBytes && (type & TYPE_SUBS)) {
            if (p.at(q) != '\\') break;
            if (--numBytes == 0) break;
            if (p.at(q + 1) != '\n') break;
            q += 2;
            if (--numBytes == 0) {
                *incomplete = true;
                break;
            }
            continue;
        }
        break;
    }
    *typePtr = type;
    return q - src;
}

int ParseAllWhiteSpace(const Parse &p, int src, int numBytes) {
    bool dummy = false;
    unsigned type = 0;
    int q = src;
    do {
        int scanned = ParseWhiteSpace(p, q, numBytes, &dummy, &type);
        q += scanned;
        numBytes -= scanned;
    } while (numBytes && p.at(q) == '\n' && (q++, numBytes--));
    return q - src;
}

int ParseHex(const Parse &p, int src, int numBytes, int *result) {
    int r = 0, q = src;
    while (numBytes--) {
        const unsigned char d = static_cast<unsigned char>(p.at(q));
        if (!std::isxdigit(d) || r > 0x10FFF) break;
        ++q;
        r <<= 4;
        if (d >= 'a') r |= 10 + d - 'a';
        else if (d >= 'A') r |= 10 + d - 'A';
        else r |= d - '0';
    }
    *result = r;
    return q - src;
}

void AppendUtf8(std::string *out, int c) {
    if (!out) return;
    const unsigned u = static_cast<unsigned>(c);
    if (u < 0x80) {
        out->push_back(static_cast<char>(u));
    } else if (u < 0x800) {
        out->push_back(static_cast<char>(0xC0 | (u >> 6)));
        out->push_back(static_cast<char>(0x80 | (u & 0x3F)));
    } else if (u < 0x10000) {
        out->push_back(static_cast<char>(0xE0 | (u >> 12)));
        out->push_back(static_cast<char>(0x80 | ((u >> 6) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | (u & 0x3F)));
    } else {
        out->push_back(static_cast<char>(0xF0 | (u >> 18)));
        out->push_back(static_cast<char>(0x80 | ((u >> 12) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | ((u >> 6) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | (u & 0x3F)));
    }
}

int Utf8Len(unsigned char c) {
    if (c < 0xC0) return 1;
    if (c < 0xE0) return 2;
    if (c < 0xF0) return 3;
    if (c < 0xF8) return 4;
    return 1;
}

// TclParseBackslash: the number of bytes read; the value goes to *out.
int Backslash(const Parse &p, int src, int numBytes, std::string *out) {
    if (numBytes == 0) return 0;
    if (numBytes == 1) {
        if (out) out->push_back('\\');
        return 1;
    }
    int q = src + 1;
    int count = 2;
    int result = 0;
    const char c = p.at(q);
    switch (c) {
    case 'a': result = 0x7; break;
    case 'b': result = 0x8; break;
    case 'f': result = 0xC; break;
    case 'n': result = 0xA; break;
    case 'r': result = 0xD; break;
    case 't': result = 0x9; break;
    case 'v': result = 0xB; break;
    case 'x':
        count += ParseHex(p, q + 1, (numBytes > 3) ? 2 : numBytes - 2, &result);
        if (count == 2) result = 'x';
        else result &= 0xFF;
        break;
    case 'u':
        count += ParseHex(p, q + 1, (numBytes > 5) ? 4 : numBytes - 2, &result);
        if (count == 2) {
            result = 'u';
        } else if (((result & 0xFC00) == 0xD800) && (count == 6) && p.at(q + 5) == '\\' && p.at(q + 6) == 'u' &&
                   numBytes >= 10) {
            int low = 0;
            const int count2 = ParseHex(p, q + 7, 4, &low);
            if (count2 == 4 && (low & 0xFC00) == 0xDC00) {
                result = ((result & 0x3FF) << 10 | (low & 0x3FF)) + 0x10000;
                count += count2 + 2;
            }
        }
        break;
    case 'U':
        count += ParseHex(p, q + 1, (numBytes > 9) ? 8 : numBytes - 2, &result);
        if (count == 2) result = 'U';
        else if ((result & ~0x7FF) == 0xD800) result = 0xFFFD;
        break;
    case '\n':
        count--;
        do {
            q++;
            count++;
        } while (count < numBytes && (p.at(q) == ' ' || p.at(q) == '\t'));
        result = ' ';
        break;
    case '\0':
        result = '\\';
        count = 1;
        break;
    default:
        if (c >= '0' && c <= '7') {
            result = c - '0';
            q++;
            if (numBytes == 2 || !(p.at(q) >= '0' && p.at(q) <= '7')) break;
            count = 3;
            result = (result << 3) + (p.at(q) - '0');
            q++;
            if (numBytes == 3 || !(p.at(q) >= '0' && p.at(q) <= '7') || result >= 0x20) break;
            count = 4;
            result = ((result << 3) + (p.at(q) - '0')) & 0xFF;
            break;
        }
        {
            int n = Utf8Len(static_cast<unsigned char>(c));
            if (n > numBytes - 1) n = numBytes - 1;
            if (n < 1) n = 1;
            if (out) out->append(p.t.substr(static_cast<size_t>(q), static_cast<size_t>(n)));
            return n + 1;
        }
    }
    if (result > 0x10FFFF) result = 0xFFFD;
    AppendUtf8(out, result);
    return count;
}

int ParseComment(Parse &p, int src, int numBytes, Command *cmd) {
    int q = src;
    while (numBytes) {
        unsigned type = 0;
        int scanned;
        do {
            scanned = ParseWhiteSpace(p, q, numBytes, &p.incomplete, &type);
            q += scanned;
            numBytes -= scanned;
        } while (numBytes && p.at(q) == '\n' && (q++, numBytes--));
        if (numBytes == 0 || p.at(q) != '#') break;
        if (cmd->comment_start < 0) cmd->comment_start = q;
        while (numBytes) {
            if (p.at(q) == '\\') {
                scanned = ParseWhiteSpace(p, q, numBytes, &p.incomplete, &type);
                if (scanned) {
                    q += scanned;
                    numBytes -= scanned;
                } else {
                    scanned = Backslash(p, q, numBytes, nullptr);
                    q += scanned;
                    numBytes -= scanned;
                }
            } else {
                q++;
                numBytes--;
                if (p.at(q - 1) == '\n') break;
            }
        }
        cmd->comment_size = q - cmd->comment_start;
    }
    return q - src;
}

bool ParseVarName(Parse &p, int start, int numBytes);

bool ParseTokens(Parse &p, int src, int numBytes, unsigned mask) {
    const int originalTokens = p.num();
    unsigned type = 0;
    while (numBytes && !((type = CharType(p.at(src))) & mask)) {
        Token tk;
        tk.start = src;
        if ((type & TYPE_SUBS) == 0) {
            while ((++src, --numBytes) && !(CharType(p.at(src)) & (mask | TYPE_SUBS))) {
            }
            tk.type = TokType::Text;
            tk.size = src - tk.start;
            p.tokens->push_back(tk);
        } else if (p.at(src) == '$') {
            const int varToken = p.num();
            if (!ParseVarName(p, src, numBytes)) return false;
            src += p.tok(varToken).size;
            numBytes -= p.tok(varToken).size;
        } else if (p.at(src) == '[') {
            src++;
            numBytes--;
            if (p.depth >= kMaxDepth) {
                p.fail("too many nested commands");
                p.term = src;
                return false;
            }
            while (true) {
                Command nested;
                Parse np;
                np.t = p.t;
                np.err = p.err;
                np.depth = p.depth + 1;
                const int curEnd = src + numBytes;
                np.end = curEnd;
                if (!ParseCommandImpl(np, src, numBytes, true, &nested)) {
                    p.term = np.term;
                    p.incomplete = np.incomplete;
                    return false;
                }
                src = nested.command_start + nested.command_size;
                numBytes = curEnd - src;
                if (np.term < p.end && p.at(np.term) == ']' && !np.incomplete) break;
                if (numBytes == 0) {
                    p.fail("missing close-bracket");
                    p.term = tk.start;
                    p.incomplete = true;
                    return false;
                }
            }
            tk.type = TokType::Command;
            tk.size = src - tk.start;
            p.tokens->push_back(tk);
        } else if (p.at(src) == '\\') {
            tk.size = Backslash(p, src, numBytes, nullptr);
            if (tk.size == 1) {
                tk.type = TokType::Text;
                p.tokens->push_back(tk);
                src++;
                numBytes--;
                continue;
            }
            if (p.at(src + 1) == '\n') {
                if (numBytes == 2) p.incomplete = true;
                if (mask & TYPE_SPACE) {
                    if (p.num() == originalTokens) {
                        tk.type = TokType::Text;
                        tk.size = 0;
                        p.tokens->push_back(tk);
                        p.term = src;
                        return true;
                    }
                    break;
                }
            }
            tk.type = TokType::Bs;
            p.tokens->push_back(tk);
            src += tk.size;
            numBytes -= tk.size;
        } else {  // '\0'
            tk.type = TokType::Text;
            tk.size = 1;
            p.tokens->push_back(tk);
            src++;
            numBytes--;
        }
    }
    if (p.num() == originalTokens) {
        Token tk;
        tk.start = src;
        tk.type = TokType::Text;
        tk.size = 0;
        p.tokens->push_back(tk);
    }
    p.term = src;
    return true;
}

bool ParseVarName(Parse &p, int start, int numBytes) {
    if (numBytes == 0) return false;
    int src = start;
    const int varIndex = p.num();
    Token var;
    var.type = TokType::Variable;
    var.start = src;
    p.tokens->push_back(var);
    src++;
    numBytes--;
    auto justADollarSign = [&]() {
        p.tokens->resize(static_cast<size_t>(varIndex + 1));
        Token &v = p.tok(varIndex);
        v.type = TokType::Text;
        v.size = 1;
        v.num_components = 0;
        return true;
    };
    if (numBytes == 0) return justADollarSign();
    Token name;
    name.type = TokType::Text;
    name.start = src;
    if (p.at(src) == '{') {
        src++;
        numBytes--;
        name.start = src;
        while (numBytes && p.at(src) != '}') {
            numBytes--;
            src++;
        }
        if (numBytes == 0) {
            p.fail("missing close-brace for variable name");
            p.term = name.start - 1;
            p.incomplete = true;
            return false;
        }
        name.size = src - name.start;
        p.tokens->push_back(name);
        src++;
    } else {
        while (numBytes) {
            if (IsBareword(p.at(src))) {
                src++;
                numBytes--;
                continue;
            }
            if (p.at(src) == ':' && numBytes != 1 && p.at(src + 1) == ':') {
                src += 2;
                numBytes -= 2;
                while (numBytes && p.at(src) == ':') {
                    src++;
                    numBytes--;
                }
                continue;
            }
            break;
        }
        const bool array = numBytes && p.at(src) == '(';
        name.size = src - name.start;
        if (name.size == 0 && !array) return justADollarSign();
        p.tokens->push_back(name);
        if (array) {
            if (!ParseTokens(p, src + 1, numBytes - 1, TYPE_CLOSE_PAREN)) return false;
            if (p.term == src + numBytes || p.at(p.term) != ')') {
                p.fail("missing )");
                p.term = src;
                p.incomplete = true;
                return false;
            }
            src = p.term + 1;
        }
    }
    Token &v = p.tok(varIndex);
    v.size = src - v.start;
    v.num_components = p.num() - (varIndex + 1);
    return true;
}

bool ParseBraces(Parse &p, int start, int numBytes, int *termPtr) {
    if (numBytes == 0) return false;
    int src = start;
    const int startIndex = p.num();
    Token tk;
    tk.type = TokType::Text;
    tk.start = src + 1;
    int level = 1;
    while (true) {
        while (++src, --numBytes) {
            if (CharType(p.at(src)) != TYPE_NORMAL) break;
        }
        if (numBytes == 0) break;
        switch (p.at(src)) {
        case '{':
            level++;
            break;
        case '}':
            if (--level == 0) {
                if (src != tk.start || p.num() == startIndex) {
                    tk.size = src - tk.start;
                    p.tokens->push_back(tk);
                }
                *termPtr = src + 1;
                return true;
            }
            break;
        case '\\': {
            const int length = Backslash(p, src, numBytes, nullptr);
            if (length > 1 && p.at(src + 1) == '\n') {
                if (numBytes == 2) p.incomplete = true;
                tk.size = src - tk.start;
                if (tk.size != 0) p.tokens->push_back(tk);
                Token bs;
                bs.type = TokType::Bs;
                bs.start = src;
                bs.size = length;
                p.tokens->push_back(bs);
                src += length - 1;
                numBytes -= length - 1;
                tk = Token();
                tk.type = TokType::Text;
                tk.start = src + 1;
            } else {
                src += length - 1;
                numBytes -= length - 1;
            }
            break;
        }
        default:
            break;
        }
    }
    // missingBraceError
    p.term = start;
    p.incomplete = true;
    std::string msg = "missing close-brace";
    bool openBrace = false;
    while (--src > start) {
        const char c = p.at(src);
        if (c == '{') {
            openBrace = true;
        } else if (c == '\n') {
            openBrace = false;
        } else if (c == '#') {
            if (openBrace && IsSpace(p.at(src - 1))) {
                msg += ": possible unbalanced brace in comment";
                break;
            }
        }
    }
    p.fail(msg.c_str());
    return false;
}

bool ParseQuotedString(Parse &p, int start, int numBytes, int *termPtr) {
    if (numBytes == 0) return false;
    if (!ParseTokens(p, start + 1, numBytes - 1, TYPE_QUOTE)) return false;
    if (p.at(p.term) != '"') {
        p.fail("missing \"");
        p.term = start;
        p.incomplete = true;
        return false;
    }
    *termPtr = p.term + 1;
    return true;
}

bool ParseCommandImpl(Parse &p, int start, int numBytes, bool nested, Command *cmd) {
    std::vector<Token> &tokens = cmd->tokens;
    tokens.clear();
    p.tokens = &tokens;
    p.incomplete = false;
    cmd->comment_start = -1;
    cmd->comment_size = 0;
    cmd->num_words = 0;
    const unsigned terminators = nested ? (TYPE_COMMAND_END | TYPE_CLOSE_BRACK) : TYPE_COMMAND_END;
    int scanned = ParseComment(p, start, numBytes, cmd);
    int src = start + scanned;
    numBytes -= scanned;
    if (numBytes == 0 && nested) p.incomplete = true;
    cmd->command_start = src;
    unsigned type = 0;
    while (true) {
        bool expandWord = false;
        const int wordIndex = p.num();
        Token word;
        word.type = TokType::Word;
        scanned = ParseWhiteSpace(p, src, numBytes, &p.incomplete, &type);
        src += scanned;
        numBytes -= scanned;
        if (numBytes == 0) {
            p.term = src;
            break;
        }
        if (type & terminators) {
            p.term = src;
            src++;
            break;
        }
        word.start = src;
        tokens.push_back(word);
        cmd->num_words++;
    parseWord:
        if (p.at(src) == '"') {
            int termPtr = 0;
            if (!ParseQuotedString(p, src, numBytes, &termPtr)) goto error;
            src = termPtr;
            numBytes = p.end - src;
        } else if (p.at(src) == '{') {
            const int expIdx = wordIndex + 1;
            int termPtr = 0;
            if (!ParseBraces(p, src, numBytes, &termPtr)) goto error;
            src = termPtr;
            numBytes = p.end - src;
            if (!expandWord && p.num() - expIdx == 1 && p.tok(expIdx).size == 1 && p.at(p.tok(expIdx).start) == '*' &&
                numBytes > 0) {
                bool inc = p.incomplete;
                const int ws = ParseWhiteSpace(p, termPtr, numBytes, &inc, &type);
                p.incomplete = inc;
                if (ws == 0 && type != TYPE_COMMAND_END) {
                    expandWord = true;
                    tokens.pop_back();
                    goto parseWord;
                }
            }
        } else {
            if (!ParseTokens(p, src, numBytes, TYPE_SPACE | terminators)) goto error;
            src = p.term;
            numBytes = p.end - src;
        }
        {
            Token &w = p.tok(wordIndex);
            w.size = src - w.start;
            w.num_components = p.num() - (wordIndex + 1);
            if (expandWord) {
                bool isLiteral = true;
                for (int i = 1; i <= w.num_components; i++) {
                    if (p.tok(wordIndex + i).type != TokType::Text) {
                        isLiteral = false;
                        break;
                    }
                }
                (void)isLiteral;  // Tcl splits a literal list into words here; we keep the one word
                w.type = TokType::ExpandWord;
            } else if (w.num_components == 1 && p.tok(wordIndex + 1).type == TokType::Text) {
                w.type = TokType::SimpleWord;
            }
        }
        scanned = ParseWhiteSpace(p, src, numBytes, &p.incomplete, &type);
        if (scanned) {
            src += scanned;
            numBytes -= scanned;
            continue;
        }
        if (numBytes == 0) {
            p.term = src;
            break;
        }
        if (type & terminators) {
            p.term = src;
            src++;
            break;
        }
        if (p.at(src - 1) == '"') {
            p.fail("extra characters after close-quote");
        } else {
            p.fail("extra characters after close-brace");
        }
        p.term = src;
        goto error;
    }
    cmd->command_size = src - cmd->command_start;
    cmd->term = p.term;
    return true;
error:
    cmd->command_size = p.end - cmd->command_start;
    cmd->term = p.term;
    p.err->pos = p.term;
    p.err->incomplete = p.incomplete;
    return false;
}

// --- Expressions (tclCompExpr.c) ---------------------------------------------------

enum : unsigned char {
    NODE_TYPE = 0xC0,
    BINARY = 0x40,
    UNARY = 0x80,
    LEAF = 0xC0,
    PLUS = 1,
    MINUS = 2,
    BAREWORD = 3,
    INCOMPLETE = 4,
    INVALID = 5,
    NUMBER = LEAF | 1,
    SCRIPT = LEAF | 2,
    BOOLEAN = LEAF | BAREWORD,
    BRACED = LEAF | 4,
    VARIABLE = LEAF | 5,
    QUOTED = LEAF | 6,
    EMPTY = LEAF | 7,
    UNARY_PLUS = UNARY | PLUS,
    UNARY_MINUS = UNARY | MINUS,
    FUNCTION = UNARY | BAREWORD,
    START = UNARY | 4,
    OPEN_PAREN = UNARY | 5,
    NOT = UNARY | 6,
    BIT_NOT = UNARY | 7,
    BINARY_PLUS = BINARY | PLUS,
    BINARY_MINUS = BINARY | MINUS,
    COMMA = BINARY | 3,
    MULT = BINARY | 4,
    DIVIDE = BINARY | 5,
    MOD = BINARY | 6,
    LESS = BINARY | 7,
    GREATER = BINARY | 8,
    BIT_AND = BINARY | 9,
    BIT_XOR = BINARY | 10,
    BIT_OR = BINARY | 11,
    QUESTION = BINARY | 12,
    COLON = BINARY | 13,
    LEFT_SHIFT = BINARY | 14,
    RIGHT_SHIFT = BINARY | 15,
    LEQ = BINARY | 16,
    GEQ = BINARY | 17,
    EQUAL = BINARY | 18,
    NEQ = BINARY | 19,
    AND = BINARY | 20,
    OR = BINARY | 21,
    STREQ = BINARY | 22,
    STRNEQ = BINARY | 23,
    EXPON = BINARY | 24,
    IN_LIST = BINARY | 25,
    NOT_IN_LIST = BINARY | 26,
    CLOSE_PAREN = BINARY | 27,
    END = BINARY | 28,
};

enum : unsigned char {
    PREC_END = 1, PREC_START, PREC_CLOSE_PAREN, PREC_OPEN_PAREN, PREC_COMMA, PREC_CONDITIONAL, PREC_OR,
    PREC_AND, PREC_BIT_OR, PREC_BIT_XOR, PREC_BIT_AND, PREC_EQUAL, PREC_COMPARE, PREC_SHIFT, PREC_ADD,
    PREC_MULT, PREC_EXPON, PREC_UNARY,
};

unsigned char Prec(unsigned char lexeme) {
    switch (lexeme) {
    case BINARY_PLUS: case BINARY_MINUS: return PREC_ADD;
    case COMMA: return PREC_COMMA;
    case MULT: case DIVIDE: case MOD: return PREC_MULT;
    case LESS: case GREATER: case LEQ: case GEQ: return PREC_COMPARE;
    case BIT_AND: return PREC_BIT_AND;
    case BIT_XOR: return PREC_BIT_XOR;
    case BIT_OR: return PREC_BIT_OR;
    case QUESTION: case COLON: return PREC_CONDITIONAL;
    case LEFT_SHIFT: case RIGHT_SHIFT: return PREC_SHIFT;
    case EQUAL: case NEQ: case STREQ: case STRNEQ: case IN_LIST: case NOT_IN_LIST: return PREC_EQUAL;
    case AND: return PREC_AND;
    case OR: return PREC_OR;
    case EXPON: return PREC_EXPON;
    case CLOSE_PAREN: return PREC_CLOSE_PAREN;
    case END: return PREC_END;
    case UNARY_PLUS: case UNARY_MINUS: case FUNCTION: case NOT: case BIT_NOT: return PREC_UNARY;
    case START: return PREC_START;
    case OPEN_PAREN: return PREC_OPEN_PAREN;
    default: return 0;
    }
}

unsigned char LexemeTable(unsigned char c) {
    if (c < 0x20) return INVALID;
    switch (c) {
    case ' ': return INVALID;
    case '"': return QUOTED;
    case '#': return INVALID;
    case '$': return VARIABLE;
    case '%': return MOD;
    case '\'': return INVALID;
    case '(': return OPEN_PAREN;
    case ')': return CLOSE_PAREN;
    case '+': return PLUS;
    case ',': return COMMA;
    case '-': return MINUS;
    case '/': return DIVIDE;
    case ':': return COLON;
    case ';': return INVALID;
    case '?': return QUESTION;
    case '@': return INVALID;
    case '[': return SCRIPT;
    case '\\': return INVALID;
    case ']': return INVALID;
    case '^': return BIT_XOR;
    case '_': return INVALID;
    case '`': return INVALID;
    case '{': return BRACED;
    case '}': return INVALID;
    case '~': return BIT_NOT;
    case 0x7F: return INVALID;
    default: return 0;
    }
}

constexpr int OT_LITERAL = -3, OT_TOKENS = -2, OT_EMPTY = -1;
bool IsOperator(int l) { return l >= 0; }
bool NotOperator(int l) { return l < 0; }

struct OpNode {
    unsigned char lexeme = 0;
    unsigned char precedence = 0;
    int prev = 0;
};

bool NotAlpha(const Parse &p, int i, int numBytes, int start) {
    return numBytes == 2 || (static_cast<unsigned char>(p.at(start + 2)) & 0x80) ||
           !std::isalpha(static_cast<unsigned char>(p.at(i)));
}

int ParseLexeme(const Parse &p, int start, int numBytes, unsigned char *lexemePtr, bool *isDouble = nullptr) {
    if (numBytes == 0) {
        *lexemePtr = END;
        return 0;
    }
    const unsigned char byte = static_cast<unsigned char>(p.at(start));
    if (byte < 0x80 && LexemeTable(byte) != 0) {
        *lexemePtr = LexemeTable(byte);
        return 1;
    }
    const char c1 = numBytes > 1 ? p.at(start + 1) : '\0';
    switch (byte) {
    case '*':
        if (c1 == '*') { *lexemePtr = EXPON; return 2; }
        *lexemePtr = MULT;
        return 1;
    case '=':
        if (c1 == '=') { *lexemePtr = EQUAL; return 2; }
        *lexemePtr = INCOMPLETE;
        return 1;
    case '!':
        if (c1 == '=') { *lexemePtr = NEQ; return 2; }
        *lexemePtr = NOT;
        return 1;
    case '&':
        if (c1 == '&') { *lexemePtr = AND; return 2; }
        *lexemePtr = BIT_AND;
        return 1;
    case '|':
        if (c1 == '|') { *lexemePtr = OR; return 2; }
        *lexemePtr = BIT_OR;
        return 1;
    case '<':
        if (c1 == '<') { *lexemePtr = LEFT_SHIFT; return 2; }
        if (c1 == '=') { *lexemePtr = LEQ; return 2; }
        *lexemePtr = LESS;
        return 1;
    case '>':
        if (c1 == '>') { *lexemePtr = RIGHT_SHIFT; return 2; }
        if (c1 == '=') { *lexemePtr = GEQ; return 2; }
        *lexemePtr = GREATER;
        return 1;
    case 'i':
        if (numBytes > 1 && c1 == 'n' && NotAlpha(p, start + 2, numBytes, start)) {
            *lexemePtr = IN_LIST;
            return 2;
        }
        break;
    case 'e':
        if (numBytes > 1 && c1 == 'q' && NotAlpha(p, start + 2, numBytes, start)) {
            *lexemePtr = STREQ;
            return 2;
        }
        break;
    case 'n':
        if (numBytes > 1 && NotAlpha(p, start + 2, numBytes, start)) {
            if (c1 == 'e') { *lexemePtr = STRNEQ; return 2; }
            if (c1 == 'i') { *lexemePtr = NOT_IN_LIST; return 2; }
        }
        break;
    default:
        break;
    }
    bool dbl = false;
    const int end = ParseNumber(p.t, start, start + numBytes, &dbl);
    if (end >= 0) {
        bool number = false;
        if (end < start + numBytes && !IsBareword(p.at(end))) {
            number = true;
        } else {
            if (dbl) {
                for (int q = start; q < end; q++) {
                    if (!IsBareword(p.at(q))) {
                        number = true;
                        break;
                    }
                }
            }
            if (!number) {
                unsigned char lex = 0;
                ParseLexeme(p, end, numBytes - (end - start), &lex);
                if ((NODE_TYPE & lex) == BINARY) number = true;
            }
        }
        if (number) {
            *lexemePtr = NUMBER;
            if (isDouble) *isDouble = dbl;
            return end - start;
        }
    }
    if (!IsBareword(p.at(start)) || p.at(start) == '_') {
        int n = Utf8Len(byte);
        if (n > numBytes) n = numBytes;
        *lexemePtr = INVALID;
        return n;
    }
    int e = start;
    int nb = numBytes;
    while (nb && IsBareword(p.at(e))) {
        e++;
        nb--;
    }
    *lexemePtr = BAREWORD;
    return e - start;
}

std::string Slice(const Parse &p, int start, int n) {
    if (n <= 0) return "";
    return std::string(p.t.substr(static_cast<size_t>(start), static_cast<size_t>(n)));
}

}  // namespace

bool IsSpace(char c) { return (CharType(c) & TYPE_SPACE) || c == '\n'; }

bool IsBareword(char c) {
    const unsigned char b = static_cast<unsigned char>(c);
    if (b < '0' || b > 'z') return false;
    if (b <= '9' || b >= 'a') return true;
    if (b == '_') return true;
    return b >= 'A' && b <= 'Z';
}

int ParseBackslash(std::string_view text, int pos, int end, std::string *out) {
    Parse p;
    p.t = text;
    p.end = end;
    return Backslash(p, pos, end - pos, out);
}

bool ParseCommand(std::string_view text, int start, int end, bool nested, Command *out, Error *err) {
    Parse p;
    p.t = text;
    p.end = end;
    p.err = err;
    return ParseCommandImpl(p, start, end - start, nested, out);
}

Script ParseScript(std::string_view text, int start, int end, bool nested) {
    Script s;
    int src = start;
    while (true) {
        Command cmd;
        Parse p;
        p.t = text;
        p.end = end;
        p.err = &s.error;
        if (!ParseCommandImpl(p, src, end - src, nested, &cmd)) {
            s.end = end;
            return s;
        }
        const int next = cmd.command_start + cmd.command_size;
        const bool closed = nested && p.term < end && p.at(p.term) == ']' && !p.incomplete;
        if (cmd.num_words > 0 || cmd.comment_start >= 0 || !cmd.tokens.empty()) s.commands.push_back(std::move(cmd));
        if (closed) {
            s.end = p.term;
            return s;
        }
        if (next >= end) {
            if (nested) {
                s.error.failed = true;
                s.error.message = "missing close-bracket";
                s.error.pos = start - 1;
                s.error.incomplete = true;
            }
            s.end = end;
            return s;
        }
        src = next;
    }
}

int ParseNumber(std::string_view text, int pos, int end, bool *is_double) {
    enum State {
        INITIAL, SIGNUM, ZERO, ZERO_X, ZERO_O, ZERO_B, BIN, HEXADECIMAL, OCTAL, BAD_OCTAL, DECIMAL,
        LEADING_RADIX_POINT, FRACTION, EXPONENT_START, EXPONENT_SIGNUM, EXPONENT, sI, sIN, sINF, sINFI, sINFIN,
        sINFINI, sINFINIT, sINFINITY, sN, sNA, sNAN, sNANPAREN, sNANHEX, sNANFINISH,
    };
    State state = INITIAL, acceptState = INITIAL;
    int p = pos, acceptPoint = pos;
    int numSigDigs = 0;
    bool explicitOctal = false;
    auto ch = [&](int i) { return i < end ? text[static_cast<size_t>(i)] : '\0'; };
    auto isdig = [](char c) { return c >= '0' && c <= '9'; };
    auto accept = [&]() {
        acceptState = state;
        acceptPoint = p;
    };
    while (true) {
        const char c = ch(p);
        switch (state) {
        case INITIAL:
            if (IsSpace(c) && c != '\0') goto endgame;
            if (c == '+' || c == '-') {
                state = SIGNUM;
                break;
            }
            [[fallthrough]];
        case SIGNUM:
            if (c == '0') {
                state = ZERO;
                break;
            } else if (isdig(c)) {
                numSigDigs = 1;
                state = DECIMAL;
                break;
            } else if (c == '.') {
                state = LEADING_RADIX_POINT;
                break;
            } else if (c == 'I' || c == 'i') {
                state = sI;
                break;
            } else if (c == 'N' || c == 'n') {
                state = sN;
                break;
            }
            goto endgame;
        case ZERO:
            accept();
            if (c == 'x' || c == 'X') {
                state = ZERO_X;
                break;
            }
            if (c == 'b' || c == 'B') {
                state = ZERO_B;
                break;
            }
            if (c == 'o' || c == 'O') {
                explicitOctal = true;
                state = ZERO_O;
                break;
            }
            goto octal_digits;
        case OCTAL:
            accept();
            [[fallthrough]];
        case ZERO_O:
        octal_digits:
            if (c >= '0' && c <= '7') {
                state = OCTAL;
                break;
            }
            [[fallthrough]];
        case BAD_OCTAL:
            if (explicitOctal) goto endgame;
            if (isdig(c)) {
                state = BAD_OCTAL;
                break;
            } else if (c == '.') {
                state = FRACTION;
                break;
            } else if (c == 'E' || c == 'e') {
                state = EXPONENT_START;
                break;
            }
            goto endgame;
        case HEXADECIMAL:
            accept();
            [[fallthrough]];
        case ZERO_X:
            if (std::isxdigit(static_cast<unsigned char>(c))) {
                state = HEXADECIMAL;
                break;
            }
            goto endgame;
        case BIN:
            accept();
            [[fallthrough]];
        case ZERO_B:
            if (c == '0' || c == '1') {
                state = BIN;
                break;
            }
            goto endgame;
        case DECIMAL:
            accept();
            if (isdig(c)) {
                state = DECIMAL;
                break;
            } else if (c == '.') {
                state = FRACTION;
                break;
            } else if (c == 'E' || c == 'e') {
                state = EXPONENT_START;
                break;
            }
            goto endgame;
        case FRACTION:
            accept();
            if (c == 'E' || c == 'e') {
                state = EXPONENT_START;
                break;
            }
            [[fallthrough]];
        case LEADING_RADIX_POINT:
            if (isdig(c)) {
                state = FRACTION;
                break;
            }
            goto endgame;
        case EXPONENT_START:
            if (c == '+' || c == '-') {
                state = EXPONENT_SIGNUM;
                break;
            }
            [[fallthrough]];
        case EXPONENT_SIGNUM:
            if (isdig(c)) {
                state = EXPONENT;
                break;
            }
            goto endgame;
        case EXPONENT:
            accept();
            if (isdig(c)) break;
            goto endgame;
        case sI:
            if (c == 'n' || c == 'N') { state = sIN; break; }
            goto endgame;
        case sIN:
            if (c == 'f' || c == 'F') { state = sINF; break; }
            goto endgame;
        case sINF:
            accept();
            if (c == 'i' || c == 'I') { state = sINFI; break; }
            goto endgame;
        case sINFI:
            if (c == 'n' || c == 'N') { state = sINFIN; break; }
            goto endgame;
        case sINFIN:
            if (c == 'i' || c == 'I') { state = sINFINI; break; }
            goto endgame;
        case sINFINI:
            if (c == 't' || c == 'T') { state = sINFINIT; break; }
            goto endgame;
        case sINFINIT:
            if (c == 'y' || c == 'Y') { state = sINFINITY; break; }
            goto endgame;
        case sN:
            if (c == 'a' || c == 'A') { state = sNA; break; }
            goto endgame;
        case sNA:
            if (c == 'n' || c == 'N') { state = sNAN; break; }
            goto endgame;
        case sNAN:
            accept();
            if (c == '(') { state = sNANPAREN; break; }
            goto endgame;
        case sNANHEX:
            if (c == ')') { state = sNANFINISH; break; }
            [[fallthrough]];
        case sNANPAREN:
            if (IsSpace(c) && c != '\0') break;
            if (numSigDigs < 13 && std::isxdigit(static_cast<unsigned char>(c))) {
                numSigDigs++;
                state = sNANHEX;
                break;
            }
            goto endgame;
        case sNANFINISH:
        case sINFINITY:
            accept();
            goto endgame;
        }
        if (p >= end) goto endgame;  // Tcl reads the NUL and stops at it
        p++;
    }
endgame:
    if (acceptState == INITIAL) return -1;
    if (is_double) {
        *is_double = acceptState == FRACTION || acceptState == EXPONENT || acceptState == sINF ||
                     acceptState == sINFINITY || acceptState == sNAN || acceptState == sNANFINISH;
    }
    return acceptPoint;
}

bool IsBooleanString(std::string_view s) {
    if (s.empty()) return false;
    {
        bool dbl = false;
        const int e = ParseNumber(s, 0, static_cast<int>(s.size()), &dbl);
        if (e == static_cast<int>(s.size())) return true;
    }
    std::string l;
    for (char c : s) l.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    auto prefix = [&](const char *word, size_t min) {
        const std::string_view w(word);
        return l.size() >= min && l.size() <= w.size() && w.compare(0, l.size(), l) == 0;
    };
    return prefix("true", 1) || prefix("false", 1) || prefix("yes", 1) || prefix("no", 1) || prefix("on", 2) ||
           prefix("off", 2);
}

ExprResult ParseExpr(std::string_view text, int startPos, int endPos) {
    ExprResult r;
    Parse p;
    p.t = text;
    p.end = endPos;
    p.tokens = &r.tokens;
    p.err = &r.error;
    const int exprStart = startPos;
    int start = startPos;
    int numBytes = endPos - startPos;
    std::vector<OpNode> nodes;
    nodes.reserve(64);
    OpNode n0;
    n0.lexeme = START;
    n0.precedence = Prec(START);
    nodes.push_back(n0);
    int incomplete = 0, lastParsed = 0;
    int complete = OT_EMPTY;
    std::string msg;
    std::string post;
    bool haveMsg = false;
    int scanned = 0;
    bool insertMark = false;
    const int limit = 25;
    const char *mark = "_@_";
    auto setMsg = [&](const std::string &m) {
        msg = m;
        haveMsg = true;
    };
    auto quoteN = [&](int s, int n) {
        // "%.*s%s" with the 25-byte limit
        if (n < limit) return Slice(p, s, n);
        return Slice(p, s, limit - 3) + "...";
    };
    while (true) {
        OpNode node;
        int nodeIndex = static_cast<int>(nodes.size());
        scanned = ParseAllWhiteSpace(p, start, numBytes);
        start += scanned;
        numBytes -= scanned;
        unsigned char lexeme = 0;
        scanned = ParseLexeme(p, start, numBytes, &lexeme);
        if ((NODE_TYPE & lexeme) == 0) {
            switch (lexeme) {
            case INVALID:
                setMsg("invalid character \"" + Slice(p, start, scanned) + "\"");
                goto error;
            case INCOMPLETE:
                setMsg("incomplete operator \"" + Slice(p, start, scanned) + "\"");
                goto error;
            case BAREWORD: {
                const int after = start + scanned + ParseAllWhiteSpace(p, start + scanned, numBytes - scanned);
                const std::string word = Slice(p, start, scanned);
                if (p.at(after) == '(') {
                    lexeme = FUNCTION;
                    r.functions.push_back({start, scanned});
                } else if (IsBooleanString(word)) {
                    lexeme = BOOLEAN;
                } else {
                    const std::string w = quoteN(start, scanned);
                    setMsg("invalid bareword \"" + w + "\"");
                    post = "should be \"$" + w + "\" or \"{" + w + "}\" or \"" + w + "(...)\" or ...";
                    if (p.at(start) == '0') {
                        const int stop = ParseNumber(text, start, start + scanned);
                        const int st = stop < 0 ? start : stop;
                        if (std::isdigit(static_cast<unsigned char>(p.at(st))) || st == start + 1) {
                            const char c1 = p.at(start + 1);
                            if (c1 == 'b') post += " (invalid binary number?)";
                            else if (c1 == 'o') post += " (invalid octal number?)";
                            else if (std::isdigit(static_cast<unsigned char>(c1))) post += " (invalid octal number?)";
                        }
                    }
                    goto error;
                }
                break;
            }
            case PLUS:
            case MINUS:
                lexeme = static_cast<unsigned char>(lexeme | (IsOperator(lastParsed) ? UNARY : BINARY));
                break;
            default:
                break;
            }
        }
        switch (NODE_TYPE & lexeme) {
        case LEAF: {
            if (NotOperator(lastParsed)) {
                setMsg(std::string("missing operator at ") + mark);
                scanned = 0;
                insertMark = true;
                goto error;
            }
            if (lexeme == NUMBER || lexeme == BOOLEAN) {
                r.literals.push_back({start, scanned});
                complete = lastParsed = OT_LITERAL;
                start += scanned;
                numBytes -= scanned;
                continue;
            }
            const int wordIndex = p.num();
            Token word;
            word.type = TokType::Word;
            word.start = start;
            r.tokens.push_back(word);
            bool ok = true;
            int end = start;
            switch (lexeme) {
            case QUOTED:
                ok = ParseQuotedString(p, start, numBytes, &end);
                scanned = end - start;
                break;
            case BRACED:
                ok = ParseBraces(p, start, numBytes, &end);
                scanned = end - start;
                break;
            case VARIABLE:
                ok = ParseVarName(p, start, numBytes);
                if (ok && p.tok(wordIndex + 1).type != TokType::Variable) {
                    setMsg("invalid character \"$\"");
                    goto error;
                }
                if (ok) scanned = p.tok(wordIndex + 1).size;
                break;
            case SCRIPT: {
                Token cmdTok;
                cmdTok.type = TokType::Command;
                cmdTok.start = start;
                const int stop = start + numBytes;
                start++;
                while (true) {
                    Command nested;
                    Parse np;
                    np.t = p.t;
                    np.end = stop;
                    np.err = p.err;
                    if (!ParseCommandImpl(np, start, stop - start, true, &nested)) {
                        p.term = np.term;
                        p.incomplete = np.incomplete;
                        ok = false;
                        break;
                    }
                    start = nested.command_start + nested.command_size;
                    if (np.term < stop && p.at(np.term) == ']' && !np.incomplete) break;
                    if (start == stop) {
                        setMsg("missing close-bracket");
                        p.term = cmdTok.start;
                        p.incomplete = true;
                        ok = false;
                        break;
                    }
                }
                const int e = start;
                start = cmdTok.start;
                scanned = e - start;
                cmdTok.size = scanned;
                r.tokens.push_back(cmdTok);
                break;
            }
            default:
                break;
            }
            if (!ok) {
                // The script parser's message (missing ", missing close-brace, ...) stands.
                if (!haveMsg) {
                    msg = r.error.message;
                    haveMsg = true;
                }
                start = p.term;
                scanned = p.incomplete ? 1 : 0;
                goto error;
            }
            Token &w = r.tokens[static_cast<size_t>(wordIndex)];
            w.size = scanned;
            w.num_components = p.num() - wordIndex - 1;
            complete = lastParsed = OT_TOKENS;
            break;
        }
        case UNARY:
            if (NotOperator(lastParsed)) {
                setMsg(std::string("missing operator at ") + mark);
                scanned = 0;
                insertMark = true;
                goto error;
            }
            node.lexeme = lexeme;
            node.precedence = Prec(lexeme);
            node.prev = incomplete;
            nodes.push_back(node);
            incomplete = lastParsed = nodeIndex;
            break;
        case BINARY: {
            const unsigned char precedence = Prec(lexeme);
            if (IsOperator(lastParsed)) {
                const OpNode &m1 = nodes[static_cast<size_t>(nodeIndex - 1)];
                if (lexeme == CLOSE_PAREN && m1.lexeme == OPEN_PAREN) {
                    if (nodeIndex >= 2 && nodes[static_cast<size_t>(nodeIndex - 2)].lexeme == FUNCTION) {
                        scanned = 0;
                        complete = lastParsed = OT_EMPTY;
                        break;
                    }
                    setMsg(std::string("empty subexpression at ") + mark);
                    scanned = 0;
                    insertMark = true;
                    goto error;
                }
                if (m1.precedence > precedence) {
                    if (m1.lexeme == OPEN_PAREN) {
                        setMsg("unbalanced open paren");
                    } else if (m1.lexeme == COMMA) {
                        setMsg(std::string("missing function argument at ") + mark);
                        scanned = 0;
                        insertMark = true;
                    } else if (m1.lexeme == START) {
                        setMsg("empty expression");
                    }
                } else if (lexeme == CLOSE_PAREN) {
                    setMsg("unbalanced close paren");
                } else if (lexeme == COMMA && m1.lexeme == OPEN_PAREN && nodeIndex >= 2 &&
                           nodes[static_cast<size_t>(nodeIndex - 2)].lexeme == FUNCTION) {
                    setMsg(std::string("missing function argument at ") + mark);
                    scanned = 0;
                    insertMark = true;
                }
                if (!haveMsg) {
                    setMsg(std::string("missing operand at ") + mark);
                    scanned = 0;
                    insertMark = true;
                }
                goto error;
            }
            int incIdx = incomplete;
            while (true) {
                OpNode &inc = nodes[static_cast<size_t>(incIdx = incomplete)];
                if (inc.precedence < precedence) break;
                if (inc.precedence == precedence) {
                    if (lexeme == EXPON) break;
                    if (inc.lexeme == QUESTION &&
                        (NotOperator(complete) || nodes[static_cast<size_t>(complete)].lexeme != COLON))
                        break;
                    if (inc.lexeme == COLON && lexeme == QUESTION) break;
                }
                if (inc.lexeme == OPEN_PAREN && lexeme != CLOSE_PAREN) {
                    setMsg("unbalanced open paren");
                    goto error;
                }
                if (inc.lexeme == QUESTION &&
                    (NotOperator(complete) || nodes[static_cast<size_t>(complete)].lexeme != COLON)) {
                    setMsg(std::string("missing operator \":\" at ") + mark);
                    scanned = 0;
                    insertMark = true;
                    goto error;
                }
                if (IsOperator(complete) && nodes[static_cast<size_t>(complete)].lexeme == COLON &&
                    inc.lexeme != QUESTION) {
                    setMsg("unexpected operator \":\" without preceding \"?\"");
                    goto error;
                }
                if (inc.lexeme == START) {
                    r.error = Error();
                    return r;
                }
                complete = incomplete;
                incomplete = inc.prev;
                if (inc.lexeme == OPEN_PAREN) break;
            }
            const OpNode &incNode = nodes[static_cast<size_t>(incIdx)];
            if (lexeme == CLOSE_PAREN && incNode.lexeme != OPEN_PAREN) {
                setMsg("unbalanced close paren");
                goto error;
            }
            if (lexeme == COMMA) {
                if (incNode.lexeme != OPEN_PAREN || incIdx < 1 ||
                    nodes[static_cast<size_t>(incIdx - 1)].lexeme != FUNCTION) {
                    setMsg("unexpected \",\" outside function argument list");
                    goto error;
                }
            }
            if (IsOperator(complete) && nodes[static_cast<size_t>(complete)].lexeme == COLON) {
                setMsg("unexpected operator \":\" without preceding \"?\"");
                goto error;
            }
            if (lexeme == CLOSE_PAREN) break;
            node.lexeme = lexeme;
            node.precedence = precedence;
            node.prev = incomplete;
            nodes.push_back(node);
            incomplete = lastParsed = nodeIndex;
            break;
        }
        default:
            break;
        }
        start += scanned;
        numBytes -= scanned;
    }
error : {
    // "\nin expression \"...\"" exactly as Tcl formats it.
    const int strStart = exprStart;
    const int strEnd = endPos;
    std::string ctx = "\nin expression \"";
    if (start - limit < strStart) {
        ctx += Slice(p, strStart, start - strStart);
    } else {
        ctx += "..." + Slice(p, start - limit + 3, limit - 3);
    }
    ctx += (scanned < limit) ? Slice(p, start, scanned) : Slice(p, start, limit - 3) + "...";
    if (insertMark) ctx += mark;
    if (start + scanned + limit > strEnd) {
        ctx += Slice(p, start + scanned, strEnd - start - scanned);
    } else {
        ctx += Slice(p, start + scanned, limit - 3) + "...";
    }
    ctx += "\"";
    r.error.failed = true;
    r.error.message = msg + ctx;
    if (!post.empty()) r.error.message += ";\n" + post;
    r.error.pos = start;
    r.error.incomplete = p.incomplete;
    return r;
}
}

bool SplitList(std::string_view text, int start, int end, std::vector<ListElem> *out, Error *err) {
    Parse pp;
    pp.t = text;
    pp.end = end;
    int p = start;
    const int limit = end;
    auto at = [&](int i) { return pp.at(i); };
    while (p < limit) {
        while (p < limit && IsSpace(at(p))) p++;
        if (p == limit) break;
        ListElem el;
        int openBraces = 0;
        bool inQuotes = false;
        if (at(p) == '{') {
            openBraces = 1;
            el.braced = true;
            p++;
        } else if (at(p) == '"') {
            inQuotes = true;
            el.quoted = true;
            p++;
        }
        const int elemStart = p;
        int size = -1;
        while (p < limit) {
            const char c = at(p);
            if (c == '{') {
                if (openBraces != 0) openBraces++;
            } else if (c == '}') {
                if (openBraces > 1) {
                    openBraces--;
                } else if (openBraces == 1) {
                    size = p - elemStart;
                    p++;
                    if (p >= limit || IsSpace(at(p))) break;
                    int p2 = p;
                    while (p2 < limit && !IsSpace(at(p2)) && p2 < p + 20) p2++;
                    err->failed = true;
                    err->message = "list element in braces followed by \"" + Slice(pp, p, p2 - p) + "\" instead of space";
                    err->pos = p;
                    return false;
                }
            } else if (c == '\\') {
                if (openBraces == 0) el.literal = false;
                const int n = Backslash(pp, p, limit - p, nullptr);
                p += n - 1;
            } else if (c == '"') {
                if (inQuotes) {
                    size = p - elemStart;
                    p++;
                    if (p >= limit || IsSpace(at(p))) break;
                    int p2 = p;
                    while (p2 < limit && !IsSpace(at(p2)) && p2 < p + 20) p2++;
                    err->failed = true;
                    err->message = "list element in quotes followed by \"" + Slice(pp, p, p2 - p) + "\" instead of space";
                    err->pos = p;
                    return false;
                }
            } else if (IsSpace(c)) {
                if (openBraces == 0 && !inQuotes) {
                    size = p - elemStart;
                    break;
                }
            }
            p++;
        }
        if (size < 0) {
            if (openBraces != 0) {
                err->failed = true;
                err->message = "unmatched open brace in list";
                err->pos = elemStart - 1;
                return false;
            }
            if (inQuotes) {
                err->failed = true;
                err->message = "unmatched open quote in list";
                err->pos = elemStart - 1;
                return false;
            }
            size = p - elemStart;
        }
        el.start = elemStart;
        el.size = size;
        out->push_back(el);
    }
    return true;
}

std::string ElementValue(std::string_view text, const ListElem &e) {
    if (e.braced || e.literal) return std::string(text.substr(static_cast<size_t>(e.start), static_cast<size_t>(e.size)));
    std::string out;
    Parse p;
    p.t = text;
    p.end = e.start + e.size;
    for (int i = e.start; i < p.end;) {
        if (p.at(i) == '\\') {
            i += Backslash(p, i, p.end - i, &out);
        } else {
            out.push_back(p.at(i));
            i++;
        }
    }
    return out;
}

bool WordLiteral(std::string_view text, const std::vector<Token> &tokens, size_t word, std::string *out) {
    if (word >= tokens.size()) return false;
    const Token &w = tokens[word];
    if (w.type == TokType::ExpandWord) return false;
    out->clear();
    Parse p;
    p.t = text;
    p.end = static_cast<int>(text.size());
    for (size_t i = word + 1; i <= word + static_cast<size_t>(w.num_components) && i < tokens.size(); i++) {
        const Token &t = tokens[i];
        if (t.type == TokType::Text) {
            out->append(text.substr(static_cast<size_t>(t.start), static_cast<size_t>(t.size)));
        } else if (t.type == TokType::Bs) {
            Backslash(p, t.start, t.size, out);
        } else {
            return false;
        }
    }
    return true;
}

}  // namespace tclparse
