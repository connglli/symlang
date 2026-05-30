#include "reify/func_desc.hpp"

#include <cctype>
#include <fstream>
#include <sstream>

#include "ast/sir_printer.hpp"

namespace symir::reify {

  // ---------------------------------------------------------------------------
  // Helpers
  // ---------------------------------------------------------------------------

  static std::string jsonEscape(const std::string &s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c: s) {
      if (c == '"' || c == '\\')
        out.push_back('\\');
      out.push_back(c);
    }
    return out;
  }

  // ---------------------------------------------------------------------------
  // Writer
  // ---------------------------------------------------------------------------

  void writeFuncDescriptor(const std::filesystem::path &outPath, const FuncDescriptor &d) {
    std::ofstream ofs(outPath);
    if (!ofs)
      throw std::runtime_error("cannot open descriptor for write: " + outPath.string());

    ofs << "{\n";
    ofs << "  \"id\": \"" << d.id << "\",\n";
    ofs << "  \"name\": \"" << d.name << "\",\n";
    ofs << "  \"ret_type\": \"" << jsonEscape(d.retType) << "\",\n";

    ofs << "  \"params\": [";
    for (size_t i = 0; i < d.params.size(); ++i) {
      if (i)
        ofs << ", ";
      ofs << "{\"name\": \"" << d.params[i].name << "\", "
          << "\"type\": \"" << jsonEscape(d.params[i].type) << "\"}";
    }
    ofs << "],\n";

    ofs << "  \"path\": [";
    for (size_t i = 0; i < d.path.size(); ++i) {
      if (i)
        ofs << ", ";
      ofs << "\"" << d.path[i] << "\"";
    }
    ofs << "],\n";

    ofs << "  \"structs\": [";
    for (size_t i = 0; i < d.structs.size(); ++i) {
      if (i)
        ofs << ", ";
      const auto &s = d.structs[i];
      ofs << "{\"name\": \"" << s.name << "\", \"fields\": [";
      for (size_t j = 0; j < s.fields.size(); ++j) {
        if (j)
          ofs << ", ";
        ofs << "{\"name\": \"" << s.fields[j].name << "\", "
            << "\"type\": \"" << jsonEscape(s.fields[j].type) << "\"}";
      }
      ofs << "]}";
    }
    ofs << "],\n";

    ofs << "  \"realizations\": [";
    for (size_t i = 0; i < d.realizations.size(); ++i) {
      if (i)
        ofs << ", ";
      const auto &r = d.realizations[i];
      ofs << "{\"file\": \"" << jsonEscape(r.file) << "\", \"params\": {";
      for (size_t j = 0; j < r.paramValues.size(); ++j) {
        if (j)
          ofs << ", ";
        ofs << "\"" << r.paramValues[j].first << "\": \"" << jsonEscape(r.paramValues[j].second)
            << "\"";
      }
      ofs << "}, \"syms\": {";
      for (size_t j = 0; j < r.symValues.size(); ++j) {
        if (j)
          ofs << ", ";
        ofs << "\"" << r.symValues[j].first << "\": \"" << jsonEscape(r.symValues[j].second)
            << "\"";
      }
      ofs << "}, \"ret\": \"" << jsonEscape(r.retValue) << "\"}";
    }
    ofs << "]\n";
    ofs << "}\n";
  }

  bool writeFuncDescriptorFromProgram(
      const std::filesystem::path &outPath, const std::string &funcName, const symir::Program &prog,
      const std::vector<std::string> &pathLabels,
      const std::vector<FuncDescriptor::Realization> &realizations, const std::string &genId
  ) {
    const symir::FunDecl *fn = nullptr;
    const std::string mangled = "@" + funcName;
    for (const auto &f: prog.funs)
      if (f.name.name == mangled) {
        fn = &f;
        break;
      }
    if (!fn)
      return false;

    FuncDescriptor d;
    d.id = genId;
    d.name = fn->name.name;
    d.retType = SIRPrinter::typeToString(fn->retType);
    for (const auto &p: fn->params)
      d.params.push_back({p.name.name, SIRPrinter::typeToString(p.type)});
    d.path = pathLabels;
    for (const auto &s: prog.structs) {
      FuncDescriptor::Struct sd;
      sd.name = s.name.name;
      for (const auto &f: s.fields)
        sd.fields.push_back({f.name, SIRPrinter::typeToString(f.type)});
      d.structs.push_back(std::move(sd));
    }
    d.realizations = realizations;

    writeFuncDescriptor(outPath, d);
    return true;
  }

  // ---------------------------------------------------------------------------
  // Parser
  //
  // Hand-written, intentionally permissive. The schema we emit is
  // flat: each value is either a quoted string or a homogeneous array
  // of quoted strings / object-literals with `"name": …` / `"type":
  // …` / `"kind": …` / `"fields": [...]`. No numbers, no booleans, no
  // nesting deeper than two levels. The parser walks the string once,
  // matches the keys we care about, and skips everything else.
  //
  // We deliberately don't pull in a JSON library — the schema is
  // small, the format is hand-written by the same code path that
  // reads it, and rylink shouldn't acquire a new dependency for this.
  // ---------------------------------------------------------------------------

  namespace {

    struct P {
      const std::string &s;
      size_t i = 0;

      explicit P(const std::string &in) : s(in) {}

      void skipWs() {
        while (i < s.size() && std::isspace((unsigned char) s[i]))
          ++i;
      }

      bool match(char c) {
        skipWs();
        if (i < s.size() && s[i] == c) {
          ++i;
          return true;
        }
        return false;
      }

      bool peek(char c) {
        skipWs();
        return i < s.size() && s[i] == c;
      }

      bool eof() {
        skipWs();
        return i >= s.size();
      }

      // Parse a JSON string literal. Handles `\"` and `\\` (matching
      // jsonEscape). Returns the unescaped value.
      bool parseString(std::string &out) {
        skipWs();
        if (i >= s.size() || s[i] != '"')
          return false;
        ++i;
        out.clear();
        while (i < s.size() && s[i] != '"') {
          if (s[i] == '\\' && i + 1 < s.size()) {
            out.push_back(s[i + 1]);
            i += 2;
          } else {
            out.push_back(s[i++]);
          }
        }
        if (i >= s.size())
          return false;
        ++i; // closing quote
        return true;
      }

      // Skip an entire JSON value (string, array, object, primitive).
      // Used to ignore keys we don't care about.
      bool skipValue() {
        skipWs();
        if (i >= s.size())
          return false;
        if (s[i] == '"') {
          std::string dummy;
          return parseString(dummy);
        }
        if (s[i] == '[' || s[i] == '{') {
          char open = s[i], close = (open == '[' ? ']' : '}');
          int depth = 0;
          while (i < s.size()) {
            char c = s[i++];
            if (c == '"') {
              // Skip strings without recursing.
              while (i < s.size() && s[i] != '"') {
                if (s[i] == '\\' && i + 1 < s.size())
                  i += 2;
                else
                  ++i;
              }
              if (i < s.size())
                ++i;
            } else if (c == open) {
              ++depth;
            } else if (c == close) {
              if (--depth == 0)
                return true;
            }
          }
          return false;
        }
        // Primitive (number / true / false / null) — read until comma
        // or close-brace/bracket.
        while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']')
          ++i;
        return true;
      }
    };

    // Object → {key: stringField}. Returns the map of recognised keys.
    bool parseFlatObject(P &p, FuncDescriptor::Param &out) {
      if (!p.match('{'))
        return false;
      bool needComma = false;
      while (!p.peek('}')) {
        if (needComma && !p.match(','))
          return false;
        std::string key;
        if (!p.parseString(key) || !p.match(':'))
          return false;
        if (key == "name") {
          if (!p.parseString(out.name))
            return false;
        } else if (key == "type") {
          if (!p.parseString(out.type))
            return false;
        } else {
          if (!p.skipValue())
            return false;
        }
        needComma = true;
      }
      return p.match('}');
    }

    bool parseStructObject(P &p, FuncDescriptor::Struct &out) {
      if (!p.match('{'))
        return false;
      bool needComma = false;
      while (!p.peek('}')) {
        if (needComma && !p.match(','))
          return false;
        std::string key;
        if (!p.parseString(key) || !p.match(':'))
          return false;
        if (key == "name") {
          if (!p.parseString(out.name))
            return false;
        } else if (key == "fields") {
          if (!p.match('['))
            return false;
          bool fComma = false;
          while (!p.peek(']')) {
            if (fComma && !p.match(','))
              return false;
            FuncDescriptor::Struct::Field f;
            FuncDescriptor::Param tmp; // same {name, type} shape
            if (!parseFlatObject(p, tmp))
              return false;
            f.name = std::move(tmp.name);
            f.type = std::move(tmp.type);
            out.fields.push_back(std::move(f));
            fComma = true;
          }
          if (!p.match(']'))
            return false;
        } else {
          if (!p.skipValue())
            return false;
        }
        needComma = true;
      }
      return p.match('}');
    }

  } // namespace

  std::optional<FuncDescriptor> parseFuncDescriptor(const std::string &json) {
    P p(json);
    if (!p.match('{'))
      return std::nullopt;

    FuncDescriptor d;
    bool needComma = false;
    while (!p.peek('}')) {
      if (needComma && !p.match(','))
        return std::nullopt;
      std::string key;
      if (!p.parseString(key) || !p.match(':'))
        return std::nullopt;

      if (key == "id") {
        if (!p.parseString(d.id))
          return std::nullopt;
      } else if (key == "name") {
        if (!p.parseString(d.name))
          return std::nullopt;
      } else if (key == "ret_type") {
        if (!p.parseString(d.retType))
          return std::nullopt;
      } else if (key == "params") {
        if (!p.match('['))
          return std::nullopt;
        bool c = false;
        while (!p.peek(']')) {
          if (c && !p.match(','))
            return std::nullopt;
          FuncDescriptor::Param pp;
          if (!parseFlatObject(p, pp))
            return std::nullopt;
          d.params.push_back(std::move(pp));
          c = true;
        }
        if (!p.match(']'))
          return std::nullopt;
      } else if (key == "path") {
        if (!p.match('['))
          return std::nullopt;
        bool c = false;
        while (!p.peek(']')) {
          if (c && !p.match(','))
            return std::nullopt;
          std::string v;
          if (!p.parseString(v))
            return std::nullopt;
          d.path.push_back(std::move(v));
          c = true;
        }
        if (!p.match(']'))
          return std::nullopt;
      } else if (key == "realizations") {
        if (!p.match('['))
          return std::nullopt;
        bool c = false;
        while (!p.peek(']')) {
          if (c && !p.match(','))
            return std::nullopt;
          if (!p.match('{'))
            return std::nullopt;
          FuncDescriptor::Realization rz;
          bool rc = false;
          // Each realization is `{"file": "...", "params": {...},
          // "syms": {...}, "ret": "..."}` — a flat object of string-
          // valued fields plus two key→string maps. We accept the
          // keys in any order.
          auto parseStrMap = [&](std::vector<std::pair<std::string, std::string>> &out) -> bool {
            if (!p.match('{'))
              return false;
            bool mc = false;
            while (!p.peek('}')) {
              if (mc && !p.match(','))
                return false;
              std::string k, v;
              if (!p.parseString(k) || !p.match(':') || !p.parseString(v))
                return false;
              out.emplace_back(std::move(k), std::move(v));
              mc = true;
            }
            return p.match('}');
          };
          while (!p.peek('}')) {
            if (rc && !p.match(','))
              return std::nullopt;
            std::string k;
            if (!p.parseString(k) || !p.match(':'))
              return std::nullopt;
            if (k == "file") {
              if (!p.parseString(rz.file))
                return std::nullopt;
            } else if (k == "params") {
              if (!parseStrMap(rz.paramValues))
                return std::nullopt;
            } else if (k == "syms") {
              if (!parseStrMap(rz.symValues))
                return std::nullopt;
            } else if (k == "ret") {
              if (!p.parseString(rz.retValue))
                return std::nullopt;
            } else {
              if (!p.skipValue())
                return std::nullopt;
            }
            rc = true;
          }
          if (!p.match('}'))
            return std::nullopt;
          d.realizations.push_back(std::move(rz));
          c = true;
        }
        if (!p.match(']'))
          return std::nullopt;
      } else if (key == "structs") {
        if (!p.match('['))
          return std::nullopt;
        bool c = false;
        while (!p.peek(']')) {
          if (c && !p.match(','))
            return std::nullopt;
          FuncDescriptor::Struct ss;
          if (!parseStructObject(p, ss))
            return std::nullopt;
          d.structs.push_back(std::move(ss));
          c = true;
        }
        if (!p.match(']'))
          return std::nullopt;
      } else {
        if (!p.skipValue())
          return std::nullopt;
      }
      needComma = true;
    }
    if (!p.match('}'))
      return std::nullopt;
    return d;
  }

  std::optional<FuncDescriptor> readFuncDescriptor(const std::filesystem::path &path) {
    std::ifstream ifs(path);
    if (!ifs)
      return std::nullopt;
    std::stringstream ss;
    ss << ifs.rdbuf();
    return parseFuncDescriptor(ss.str());
  }

} // namespace symir::reify
