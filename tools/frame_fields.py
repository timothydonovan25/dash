#!/usr/bin/env python3
"""Every datapoint in core::state::Frame, as a table a custom screen can bind.

    python3 tools/frame_fields.py            # regenerate
    python3 tools/frame_fields.py --check    # fail if out of date (ctest runs this)

Reads the Frame's headers under components/core/include, walks Frame member
by member (nested structs flatten to dotted paths, fixed arrays expand per
element, char arrays are strings) and writes

    components/core/src/layout_fields.gen.cpp

Each field has an id that is stable forever (docs/custom-screens.md §3). The
ids live in components/core/layout_fields.ids, committed: a new field is
appended with the next id, a field that went away stays, marked retired, so
an old .dui never binds to the wrong number. Never renumber that file.

Units come from the name (the repo puts units in names: _mph, _c, _psi ...);
tools/frame_fields.override fixes captions, units, decimals, the health
device that makes a field valid, array index names, and the blackbox column.

Anything in the headers this parser does not understand is an error, not a
skipped field: "every datapoint" has to stay true.
"""
import os
import re
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
INC = os.path.join(ROOT, "components/core/include")
OUT = os.path.join(ROOT, "components/core/src/layout_fields.gen.cpp")
IDS = os.path.join(ROOT, "components/core/layout_fields.ids")
OVERRIDE = os.path.join(ROOT, "tools/frame_fields.override")
MAX_DIM = 16

ARITH = {
    "float": "F", "double": "F",
    "bool": "B",
    "uint8_t": "I", "uint16_t": "I", "uint32_t": "I", "uint64_t": "I",
    "int8_t": "I", "int16_t": "I", "int32_t": "I", "int64_t": "I",
    "unsigned": "I", "int": "I", "size_t": "I", "char": "C",
}


class ParseError(Exception):
    pass


def strip_comments(s):
    # Literals first: a '{' in a string is not a brace, and "//" in one is
    # not a comment.
    s = re.sub(r'"(?:\\.|[^"\\\n])*"', '""', s)
    s = re.sub(r"'([^'\\\n])'", lambda m: str(ord(m.group(1))), s)
    s = re.sub(r"'\\.'", "0", s)
    s = re.sub(r"/\*.*?\*/", " ", s, flags=re.S)
    return re.sub(r"//[^\n]*", " ", s)


def match_brace(s, i):
    """s[i] == '{'; index just past its matching '}'."""
    depth = 0
    for j in range(i, len(s)):
        if s[j] == "{":
            depth += 1
        elif s[j] == "}":
            depth -= 1
            if depth == 0:
                return j + 1
    raise ParseError("unbalanced braces")


def statements(body):
    """Split a struct/namespace body into statements at depth 0."""
    out, i, start, n = [], 0, 0, len(body)
    while i < n:
        c = body[i]
        if c == "{":
            j = match_brace(body, i)
            head = body[start:i]
            # A function body ends the statement at its brace; a struct, an
            # enum or a braced initialiser runs on to its ';'.
            if "(" in head and not re.search(r"=\s*$", head) and not re.match(r"\s*(struct|enum|namespace)\b", head):
                out.append(body[start:j].strip())
                start = j
            i = j
            continue
        if c == ";":
            out.append(body[start:i].strip())
            start = i + 1
        i += 1
    tail = body[start:].strip()
    if tail:
        out.append(tail)
    return [s for s in out if s]


class Model:
    def __init__(self):
        self.structs = {}    # qualified name -> list of (type, name, dims, scope)
        self.enums = {}      # qualified name -> [(name, value)]
        self.consts = {}     # qualified name -> expression string
        self.values = {}

    # --- name lookup: struct scope first, then namespaces outward ---------
    def candidates(self, name, scope):
        parts = scope.split("::") if scope else []
        for k in range(len(parts), -1, -1):
            yield "::".join(parts[:k] + [name]) if k else name

    def find(self, table, name, scope):
        name = name.lstrip(":")
        for q in self.candidates(name, scope):
            if q in table:
                return q
        # A qualified reference from another namespace (lap::Phase from
        # core::state): match by suffix, and refuse to guess between two.
        hits = [q for q in table if q == name or q.endswith("::" + name)]
        if len(hits) == 1:
            return hits[0]
        if len(hits) > 1:
            raise ParseError(f"ambiguous {name} in {scope}: {hits}")
        return None

    def const(self, q):
        if q not in self.values:
            self.values[q] = self.value(self.consts[q], q.rsplit("::", 1)[0] if "::" in q else "")
        return self.values[q]

    def value(self, expr, scope):
        expr = expr.strip()
        expr = re.sub(r"static_cast<[^>]*>", "", expr)
        def sub(m):
            tok = m.group(0)
            if re.fullmatch(r"\d+[uUlL]*", tok):
                return re.sub(r"[uUlL]+$", "", tok)
            name = tok.lstrip(":")
            for q in self.candidates(name, scope):
                if q in self.values:
                    return str(self.values[q])
                if q in self.consts:
                    return str(self.const(q))
            q = self.find(self.values, name, scope)
            if q is not None:
                return str(self.values[q])
            q = self.find(self.consts, name, scope)
            if q is None:
                raise ParseError(f"unknown constant {tok} in {scope}")
            return str(self.const(q))
        py = re.sub(r"[A-Za-z_][\w:]*|\d+[uUlL]*", sub, expr)
        try:
            return int(eval(py, {"__builtins__": {}}))
        except Exception as e:
            raise ParseError(f"cannot evaluate {expr!r} ({py!r}) in {scope}: {e}")


def parse_body(model, body, scope, struct_name=None):
    members = []
    for st in statements(body):
        st = re.sub(r"\s+", " ", st).strip()
        if not st or st.startswith("#") or st.startswith("using ") or st.startswith("template"):
            continue
        if st in ("public:", "private:", "protected:"):
            continue
        st = re.sub(r"^(public|private|protected):\s*", "", st)
        m = re.match(r"namespace ([\w:]+) ?\{(.*)\}$", st, re.S)
        if m:
            ns = m.group(1)
            parse_body(model, m.group(2), f"{scope}::{ns}" if scope else ns)
            continue
        m = re.match(r"(struct|class) (\w+)( final)? ?\{(.*)\}", st, re.S)
        if m:
            q = f"{scope}::{m.group(2)}" if scope else m.group(2)
            model.structs[q] = parse_body(model, m.group(4), q, m.group(2))
            continue
        if re.match(r"(struct|class) \w+$", st):
            continue  # forward declaration
        m = re.match(r"enum (class )?(\w+)( ?: ?[\w:]+)? ?\{(.*)\}", st, re.S)
        if m:
            q = f"{scope}::{m.group(2)}" if scope else m.group(2)
            vals, nxt = [], 0
            for item in [x.strip() for x in m.group(4).split(",") if x.strip()]:
                if "=" in item:
                    nm, ex = [x.strip() for x in item.split("=", 1)]
                    v = model.value(ex, q)
                else:
                    nm, v = item, nxt
                vals.append((nm, v))
                model.values[f"{q}::{nm}"] = v
                if not m.group(1):  # unscoped: the names leak into the scope
                    model.values[f"{scope}::{nm}" if scope else nm] = v
                nxt = v + 1
            model.enums[q] = vals
            continue
        m = re.match(r"(?:static |inline )*constexpr [\w:<> ]+? (\w+) ?= ?(.+)$", st, re.S)
        if m:
            if "(" not in m.group(1):
                model.consts[f"{scope}::{m.group(1)}" if scope else m.group(1)] = m.group(2)
            continue
        if "(" in st or st.startswith("static ") or st.startswith("extern ") or st.startswith("friend "):
            continue  # functions, operators, statics: not data
        if struct_name is None:
            continue  # namespace-level variables are not Frame members
        # A data member: type name[dims] = init, name2 = init ...
        try:
            members.extend(parse_member(st, scope))
        except ParseError as e:
            # Only an error if Frame reaches this struct (walk() raises it).
            members.append(("<error>", str(e), [], scope))
    return members


def split_top(s, sep):
    """Split at `sep` outside braces, brackets and angle brackets."""
    out, depth, cur = [], 0, ""
    for ch in s:
        if ch in "{[(<":
            depth += 1
        elif ch in "}])>":
            depth -= 1
        if ch == sep and depth == 0:
            out.append(cur)
            cur = ""
        else:
            cur += ch
    out.append(cur)
    return out


def parse_member(st, scope):
    decls = [d.split("=", 1)[0].strip() if "=" in d else d.strip()
             for d in split_top(st, ",")]
    m = re.match(r"((?:const )?(?:unsigned |signed )?[\w:]+(?:<[^>]*>)?) (.+)$", decls[0])
    if not m:
        raise ParseError(f"cannot read member {st!r} in {scope}")
    typ = m.group(1)
    out = []
    for d in [m.group(2)] + decls[1:]:
        dm = re.match(r"(\w+)((?:\[[^\]]+\])*)$", d.strip())
        if not dm:
            raise ParseError(f"cannot read declarator {d!r} of {st!r} in {scope}")
        out.append((typ, dm.group(1), re.findall(r"\[([^\]]+)\]", dm.group(2)), scope))
    return out


def load_model():
    """Parse the headers Frame is built from: slots.hpp and what it includes,
    transitively, within core."""
    model = Model()
    todo, seen = ["core/state/slots.hpp"], set()
    while todo:
        rel = todo.pop()
        if rel in seen:
            continue
        seen.add(rel)
        path = os.path.join(INC, rel)
        raw = open(path).read()
        todo += [i for i in re.findall(r'#include "(core/[^"]+)"', raw)
                 if os.path.exists(os.path.join(INC, i))]
        src = strip_comments(raw)
        src = re.sub(r"^\s*#.*$", "", src, flags=re.M)
        try:
            parse_body(model, src, "")
        except ParseError as e:
            raise ParseError(f"components/core/include/{rel}: {e}")
    model.headers = sorted(seen)
    return model


# --- overrides ---------------------------------------------------------------

def load_override():
    ov = {"field": {}, "prefix": [], "index": {}, "skip": set()}
    for ln, raw in enumerate(open(OVERRIDE), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        words = re.findall(r'\w+="[^"]*"|\S+', line)
        kind, path, rest = words[0], words[1], words[2:]
        kv = {}
        for w in rest:
            if "=" not in w:
                raise SystemExit(f"{OVERRIDE}:{ln}: expected key=value, got {w}")
            k, v = w.split("=", 1)
            kv[k] = v.strip('"')
        if kind == "field":
            ov["field"].setdefault(path, {}).update(kv)
        elif kind == "prefix":
            ov["prefix"].append((path, kv))
        elif kind == "index":
            ov["index"][path] = kv["enum"]
        elif kind == "skip":
            ov["skip"].add(path)
        else:
            raise SystemExit(f"{OVERRIDE}:{ln}: unknown directive {kind}")
    return ov


# Unit from the name's suffix: (quantity, unit, decimals).
SUFFIX = [
    ("_mph", "Speed", "mph", 0), ("_mi", "DistMi", "mi", 1), ("_ft", "DistFt", "ft", 0),
    ("_c", "Temp", "C", 0), ("_psi", "Pressure", "psi", 1), ("_gal", "Volume", "gal", 2),
    ("_kpa", "Plain", "kPa", 0), ("_deg", "Plain", "deg", 0), ("_dps", "Plain", "dps", 0),
    ("_g", "Plain", "g", 2), ("_pct", "Plain", "%", 0), ("_ms", "Duration", "", 0),
    ("_s", "Seconds", "", 0), ("_m", "AltM", "m", 0), ("_dbm", "Plain", "dBm", 0),
    ("_bpm", "Plain", "bpm", 0), ("_v", "Plain", "V", 1), ("_ds", "Plain", "ds", 0),
    ("_e7", "Plain", "e7", 0), ("_rpm", "Rpm", "rpm", 0), ("_x10", "Plain", "x10", 0), ("_hours", "Plain", "h", 1),
]


def caption_for(name, path=""):
    idx = re.findall(r"\[([^\]]+)\]", path)
    if idx:
        # An array's element: say which, "GNSS STATE", "TO2 MS".
        return (idx[-1].upper() + " " + caption_for(name)).strip()[:15]
    base = name
    for suf, *_ in SUFFIX:
        if base.endswith(suf) and len(base) > len(suf):
            base = base[: -len(suf)]
            break
    return base.replace("_", " ").upper()[:15]


def walk(model, ov):
    fields = []

    def leaf(path, expr, typ, kind, enum_q):
        if any(path == s or path.startswith(s + ".") or path.startswith(s + "[") for s in ov["skip"]):
            return
        name = re.sub(r"\[.*?\]", "", path.rsplit(".", 1)[-1])
        q, unit, dec = "Plain", "", 1 if kind == "F" else 0
        if kind in ("F", "I"):
            for suf, qq, uu, dd in SUFFIX:
                if ("_" + name).endswith(suf):
                    q, unit, dec = qq, uu, dd
                    break
        if kind == "B" or kind == "E" or kind == "S" or kind == "C":
            q, unit, dec = "Plain", "", 0
        f = {"path": path, "expr": expr, "kind": kind, "q": q, "unit": unit,
             "dec": dec, "caption": caption_for(name, path), "dev": "", "column": "",
             "enum": enum_q}
        for pre, kv in ov["prefix"]:
            if path == pre or path.startswith(pre + ".") or path.startswith(pre + "["):
                if "dev" in kv:
                    f["dev"] = kv["dev"]
        o = ov["field"].get(path) or ov["field"].get(re.sub(r"\[[^\]]*\]", "[]", path))
        if o:
            for k in ("q", "unit", "caption", "dev", "column"):
                if k in o:
                    f[k] = o[k]
            if "dec" in o:
                f["dec"] = int(o["dec"])
            if "enum" in o:
                f["enum"] = model.find(model.enums, o["enum"], "core::state")
                if not f["enum"]:
                    raise ParseError(f"override {path}: no enum {o['enum']}")
                f["kind"] = "E"
        fields.append(f)

    def visit(typ, path, expr, dims, scope):
        key = re.sub(r"\[[^\]]*\]", "", path)
        if dims:
            if typ == "char" and len(dims) == 1:
                return leaf(path, expr, typ, "S", None)
            n = model.value(dims[0], scope)
            if n > MAX_DIM:
                if key in ov["skip"]:
                    return
                raise ParseError(f"{path}: array of {n} is over {MAX_DIM}; list it as `skip` or raise MAX_DIM")
            names = None
            if key in ov["index"]:
                eq = model.find(model.enums, ov["index"][key], scope)
                if not eq:
                    raise ParseError(f"index {key}: no enum {ov['index'][key]}")
                names = {v: nm for nm, v in model.enums[eq]}
            for i in range(n):
                label = names.get(i, str(i)) if names else str(i)
                visit(typ, f"{path}[{label}]", f"{expr}[{i}]", dims[1:], scope)
            return
        t = typ.replace("const ", "")
        if t in ARITH:
            return leaf(path, expr, t, ARITH[t], None)
        eq = model.find(model.enums, t, scope)
        if eq:
            return leaf(path, expr, t, "E", eq)
        sq = model.find(model.structs, t, scope)
        if sq:
            for (mt, mn, md, ms) in model.structs[sq]:
                if mt == "<error>":
                    raise ParseError(f"{path}: {mn}")
                visit(mt, f"{path}.{mn}" if path else mn, f"{expr}.{mn}", md, ms)
            return
        raise ParseError(f"{path}: type {typ} not understood (scope {scope})")

    frame = model.find(model.structs, "Frame", "core::state")
    if frame != "core::state::Frame":
        raise ParseError(f"Frame not found (got {frame})")
    for (mt, mn, md, ms) in model.structs[frame]:
        visit(mt, mn, f"f.{mn}", md, ms)
    return fields


# --- ids ---------------------------------------------------------------------

def assign_ids(fields):
    known, retired, top = {}, set(), 0
    if os.path.exists(IDS):
        for raw in open(IDS):
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            parts = line.split()
            i, p = int(parts[0]), parts[1]
            if i <= top:
                raise SystemExit(f"{IDS}: ids must only go up (line {raw.strip()!r})")
            top = i
            known[p] = i
            if len(parts) > 2 and parts[2] == "retired":
                retired.add(p)
    lines, now = [], {f["path"] for f in fields}
    for f in fields:
        if f["path"] not in known:
            top += 1
            known[f["path"]] = top
        f["id"] = known[f["path"]]
    for p, i in sorted(known.items(), key=lambda kv: kv[1]):
        lines.append(f"{i} {p}" + ("" if p in now else " retired"))
    head = ("# Frame field ids for custom screens (tools/frame_fields.py).\n"
            "# Stable forever: never renumber, never reuse. A field that went away\n"
            "# stays here, marked retired.\n")
    return head + "\n".join(lines) + "\n"


def cstr(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def emit(model, fields):
    enums = sorted({f["enum"] for f in fields if f["enum"]})
    out = ["// Generated by tools/frame_fields.py from the Frame's headers. Do not edit:",
           "// change the headers or tools/frame_fields.override and run the tool.",
           '#include "core/layout/fields.hpp"',
           '#include "core/state/slots.hpp"',
           "",
           "namespace core::layout {",
           "namespace {",
           "using core::state::Frame;",
           "Value num(double v) { Value x; x.kind = Value::Num; x.num = v; return x; }",
           "Value bln(bool v) { Value x; x.kind = Value::Bool; x.num = v ? 1.0 : 0.0; return x; }",
           "Value str(const char* s) { Value x; x.kind = Value::Str; x.str = s; return x; }",
           "template <typename E> Value enm(E v) { Value x; x.kind = Value::Enum; x.num = static_cast<double>(static_cast<int>(v)); return x; }",
           ""]
    for eq in enums:
        cname = "kEnum_" + eq.replace("::", "_")
        items = ", ".join(f"{{{v}, {cstr(nm)}}}" for nm, v in model.enums[eq] if not nm.startswith("k"))
        out.append(f"constexpr EnumName {cname}_items[] = {{{items}}};")
        n = len([1 for nm, _ in model.enums[eq] if not nm.startswith("k")])
        out.append(f"constexpr EnumNames {cname} = {{{cname}_items, {n}}};")
    out.append("")
    getter = {"F": "num", "I": "num", "C": "num", "B": "bln", "S": "str", "E": "enm"}
    typ = {"F": "Type::Num", "I": "Type::Int", "C": "Type::Int", "B": "Type::Bool", "S": "Type::Str", "E": "Type::Enum"}
    # In id order: find_field() is a binary search, and a field added in the
    # middle of a struct gets the highest id.
    out.append("const FieldDef kFields[] = {")
    for f in sorted(fields, key=lambda f: f["id"]):
        g = getter[f["kind"]]
        cast = "static_cast<double>" if f["kind"] in ("F", "I", "C") else ""
        expr = f["expr"]
        call = f"{g}({cast}({expr}))" if cast else f"{g}({expr})"
        en = ("&kEnum_" + f["enum"].replace("::", "_")) if f["enum"] else "nullptr"
        dev = f"static_cast<int8_t>(core::state::Dev::{f['dev']})" if f["dev"] else "-1"
        out.append(f"    {{{f['id']}, {cstr(f['path'])}, {cstr(f['caption'])}, {cstr(f['unit'])}, "
                   f"{cstr(f['column'])}, {typ[f['kind']]}, Quantity::{f['q']}, {f['dec']}, {dev}, {en}, "
                   f"[](const Frame& f) {{ return {call}; }}}},")
    out.append("};")
    out.append("}  // namespace")
    out.append("")
    out.append("const FieldDef* fields() { return kFields; }")
    out.append("size_t field_count() { return sizeof kFields / sizeof kFields[0]; }")
    out.append("")
    out.append("}  // namespace core::layout")
    return "\n".join(out) + "\n"


def main():
    check = "--check" in sys.argv
    try:
        model = load_model()
        ov = load_override()
        fields = walk(model, ov)
    except ParseError as e:
        sys.exit(f"frame_fields: {e}")
    ids = assign_ids(fields)
    gen = emit(model, fields)
    if check:
        bad = []
        if not os.path.exists(OUT) or open(OUT).read() != gen:
            bad.append(os.path.relpath(OUT, ROOT))
        if not os.path.exists(IDS) or open(IDS).read() != ids:
            bad.append(os.path.relpath(IDS, ROOT))
        if bad:
            sys.exit("frame_fields: out of date with the Frame's headers: " + ", ".join(bad) +
                     "\n  run: python3 tools/frame_fields.py")
        print(f"frame_fields: {len(fields)} fields, up to date")
        return
    open(OUT, "w").write(gen)
    open(IDS, "w").write(ids)
    print(f"frame_fields: {len(fields)} fields -> {os.path.relpath(OUT, ROOT)}")


if __name__ == "__main__":
    main()
