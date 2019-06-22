// has to go first because it violates our poisons
#include "msgpack.hpp"

#include "absl/strings/str_split.h"
#include "ast/ast.h"
#include "ast/treemap/treemap.h"
#include "common/FileOps.h"
#include "common/typecase.h"
#include "core/Names.h"
#include "main/autogen/autogen.h"

#include "CRC.h"

using namespace std;
namespace sorbet::autogen {

Definition &DefinitionRef::data(ParsedFile &pf) {
    return pf.defs[_id];
}

Reference &ReferenceRef::data(ParsedFile &pf) {
    return pf.refs[_id];
}

class AutogenWalk {
    vector<Definition> defs;
    vector<Reference> refs;
    vector<core::NameRef> requires;
    vector<DefinitionRef> nesting;
    vector<ast::Send *> ignoring;

    UnorderedMap<ast::Expression *, ReferenceRef> refMap;

    static bool ignoreChild(ast::Expression *expr) {
        bool result = false;

        typecase(
            expr, [&](ast::Send *send) { result = (send->fun == core::Names::keepForIde()); },

            [&](ast::EmptyTree *) { result = true; },

            [&](ast::InsSeq *seq) {
                result = absl::c_all_of(seq->stats, [](auto &child) { return ignoreChild(child.get()); }) &&
                         ignoreChild(seq->expr.get());
            },

            [&](ast::Expression *klass) { result = false; });
        return result;
    }

    static bool definesBehavior(ast::Expression *expr) {
        if (ignoreChild(expr)) {
            return false;
        }
        bool result = true;

        typecase(
            expr,

            [&](ast::ClassDef *klass) {
                auto *id = ast::cast_tree<ast::UnresolvedIdent>(klass->name.get());
                if (id && id->name == core::Names::singleton()) {
                    // class << self; We consider this
                    // behavior-defining. We could opt to recurse inside
                    // the inner class, but we consider there to be no
                    // valid use of `class << self` solely for namespacing,
                    // so there's no need to support that use case.
                    result = true;
                } else {
                    result = false;
                }
            },

            [&](ast::Assign *asgn) {
                if (ast::isa_tree<ast::ConstantLit>(asgn->lhs.get())) {
                    result = false;
                } else {
                    result = true;
                }
            },

            [&](ast::InsSeq *seq) {
                result = absl::c_any_of(seq->stats, [](auto &child) { return definesBehavior(child.get()); }) ||
                         definesBehavior(seq->expr.get());
            },

            [&](ast::Expression *klass) { result = true; });
        return result;
    }

    vector<core::NameRef> symbolName(core::Context ctx, core::SymbolRef sym) {
        vector<core::NameRef> out;
        while (sym.exists() && sym != core::Symbols::root()) {
            out.emplace_back(sym.data(ctx)->name);
            sym = sym.data(ctx)->owner;
        }
        reverse(out.begin(), out.end());
        return out;
    }

    vector<core::NameRef> constantName(core::Context ctx, ast::ConstantLit *cnst) {
        vector<core::NameRef> out;
        while (cnst != nullptr && cnst->original != nullptr) {
            out.emplace_back(cnst->original->cnst);
            cnst = ast::cast_tree<ast::ConstantLit>(cnst->original->scope.get());
        }
        reverse(out.begin(), out.end());
        return out;
    }

public:
    AutogenWalk() {
        auto &def = defs.emplace_back();
        def.id = 0;
        def.type = Definition::Module;
        def.defines_behavior = false;
        def.is_empty = false;
        nesting.emplace_back(def.id);
    }

    unique_ptr<ast::ClassDef> preTransformClassDef(core::Context ctx, unique_ptr<ast::ClassDef> original) {
        if (!ast::isa_tree<ast::ConstantLit>(original->name.get())) {
            return original;
        }

        // cerr << "preTransformClassDef(" << original->toString(ctx) << ")\n";

        auto &def = defs.emplace_back();
        def.id = defs.size() - 1;
        if (original->kind == ast::Class) {
            def.type = Definition::Class;
        } else {
            def.type = Definition::Module;
        }
        def.is_empty = absl::c_all_of(original->rhs, [](auto &tree) { return ignoreChild(tree.get()); });
        for (auto &ancst : original->ancestors) {
            auto *cnst = ast::cast_tree<ast::ConstantLit>(ancst.get());
            if (cnst && cnst->original != nullptr) {
                def.defines_behavior = true;
            }
        }
        for (auto &ancst : original->singletonAncestors) {
            auto *cnst = ast::cast_tree<ast::ConstantLit>(ancst.get());
            if (cnst && cnst->original != nullptr) {
                def.defines_behavior = true;
            }
        }
        if (!def.defines_behavior) {
            def.defines_behavior =
                absl::c_any_of(original->rhs, [](auto &tree) { return definesBehavior(tree.get()); });
        }

        // TODO: ref.parent_of, def.parent_ref
        // TODO: expression_range
        original->name = ast::TreeMap::apply(ctx, *this, move(original->name));
        auto it = refMap.find(original->name.get());
        ENFORCE(it != refMap.end());
        def.defining_ref = it->second;
        refs[it->second.id()].is_defining_ref = true;
        refs[it->second.id()].definitionLoc = original->loc;

        auto ait = original->ancestors.begin();
        if (original->kind == ast::Class && !original->ancestors.empty()) {
            // Handle the superclass at outer scope
            *ait = ast::TreeMap::apply(ctx, *this, move(*ait));
            ++ait;
        }
        // Then push a scope
        nesting.emplace_back(def.id);

        for (; ait != original->ancestors.end(); ++ait) {
            *ait = ast::TreeMap::apply(ctx, *this, move(*ait));
        }
        for (auto &ancst : original->singletonAncestors) {
            ancst = ast::TreeMap::apply(ctx, *this, move(ancst));
        }

        for (auto &ancst : original->ancestors) {
            auto *cnst = ast::cast_tree<ast::ConstantLit>(ancst.get());
            if (cnst == nullptr || cnst->original == nullptr) {
                // Don't include synthetic ancestors
                continue;
            }

            auto it = refMap.find(ancst.get());
            if (it == refMap.end()) {
                continue;
            }
            if (original->kind == ast::Class && &ancst == &original->ancestors.front()) {
                // superclass
                def.parent_ref = it->second;
            }
            refs[it->second.id()].parent_of = def.id;
        }

        return original;
    }

    unique_ptr<ast::Expression> postTransformClassDef(core::Context ctx, unique_ptr<ast::ClassDef> original) {
        if (!ast::isa_tree<ast::ConstantLit>(original->name.get())) {
            return original;
        }

        nesting.pop_back();

        return original;
    }

    bool isCBaseConstant(ast::ConstantLit *cnst) {
        while (cnst != nullptr && cnst->original != nullptr) {
            cnst = ast::cast_tree<ast::ConstantLit>(cnst->original->scope.get());
        }
        if (cnst && cnst->symbol == core::Symbols::root()) {
            return true;
        }
        return false;
    }

    unique_ptr<ast::Expression> postTransformConstantLit(core::Context ctx, unique_ptr<ast::ConstantLit> original) {
        if (!ignoring.empty()) {
            return original;
        }
        if (original->original == nullptr) {
            return original;
        }

        auto &ref = refs.emplace_back();
        ref.id = refs.size() - 1;
        if (isCBaseConstant(original.get())) {
            ref.scope = nesting.front();
        } else {
            ref.nesting = nesting;
            reverse(ref.nesting.begin(), ref.nesting.end());
            ref.nesting.pop_back();
            ref.scope = nesting.back();
        }
        ref.loc = original->loc;

        // This will get overridden if this loc is_defining_ref at the point
        // where we set that flag.
        ref.definitionLoc = original->loc;
        ref.name = constantName(ctx, original.get());
        auto sym = original->symbol;
        if (!sym.data(ctx)->isClass() || sym != core::Symbols::StubModule()) {
            ref.resolved = symbolName(ctx, sym);
        }
        ref.is_resolved_statically = true;
        ref.is_defining_ref = false;
        refMap[original.get()] = ref.id;
        return original;
    }

    unique_ptr<ast::Expression> postTransformAssign(core::Context ctx, unique_ptr<ast::Assign> original) {
        auto *lhs = ast::cast_tree<ast::ConstantLit>(original->lhs.get());
        if (lhs == nullptr || lhs->original == nullptr) {
            return original;
        }

        auto &def = defs.emplace_back();
        def.id = defs.size() - 1;
        auto *rhs = ast::cast_tree<ast::ConstantLit>(original->rhs.get());
        if (rhs && rhs->symbol.exists() && !rhs->symbol.data(ctx)->isTypeAlias()) {
            def.type = Definition::Alias;
            ENFORCE(refMap.count(rhs));
            def.aliased_ref = refMap[rhs];
        } else {
            def.type = Definition::Casgn;
        }
        ENFORCE(refMap.count(lhs));
        auto &ref = refs[refMap[lhs].id()];
        def.defining_ref = ref.id;
        ref.is_defining_ref = true;
        ref.definitionLoc = original->loc;

        def.defines_behavior = true;
        def.is_empty = false;

        return original;
    }

    unique_ptr<ast::Send> preTransformSend(core::Context ctx, unique_ptr<ast::Send> original) {
        if (original->fun == core::Names::keepForIde()) {
            ignoring.emplace_back(original.get());
        }
        if ((original->flags & ast::Send::PRIVATE_OK) != 0 && original->fun == core::Names::require() &&
            original->args.size() == 1) {
            auto *lit = ast::cast_tree<ast::Literal>(original->args.front().get());
            if (lit && lit->isString(ctx)) {
                requires.emplace_back(lit->asString(ctx));
            }
        }
        return original;
    }
    unique_ptr<ast::Send> postTransformSend(core::Context ctx, unique_ptr<ast::Send> original) {
        if (!ignoring.empty() && ignoring.back() == original.get()) {
            ignoring.pop_back();
        }
        return original;
    }

    ParsedFile parsedFile() {
        ParsedFile out;
        out.refs = move(refs);
        out.defs = move(defs);
        out.requires = move(requires);
        return out;
    }
};

ParsedFile Autogen::generate(core::Context ctx, ast::ParsedFile tree) {
    AutogenWalk walk;
    tree.tree = ast::TreeMap::apply(ctx, walk, move(tree.tree));
    auto pf = walk.parsedFile();
    pf.path = string(tree.file.data(ctx).path());
    auto src = tree.file.data(ctx).source();
    pf.cksum = CRC::Calculate(src.data(), src.size(), CRC::CRC_32());
    pf.tree = move(tree);
    return pf;
}

vector<core::NameRef> ParsedFile::showFullName(core::Context ctx, DefinitionRef id) {
    auto &def = id.data(*this);
    if (!def.defining_ref.exists()) {
        return {};
    }
    auto &ref = def.defining_ref.data(*this);
    auto scope = showFullName(ctx, ref.scope);
    scope.insert(scope.end(), ref.name.begin(), ref.name.end());
    return scope;
}

string ParsedFile::toString(core::Context ctx) {
    fmt::memory_buffer out;
    auto nameToString = [&](const auto &nm) -> string { return nm.data(ctx)->show(ctx); };

    fmt::format_to(out,
                   "# ParsedFile: {}\n"
                   "requires: [{}]\n"
                   "## defs:\n",
                   path, fmt::map_join(requires, ", ", nameToString));

    for (auto &def : defs) {
        string_view type;
        switch (def.type) {
            case Definition::Module:
                type = "module"sv;
                break;
            case Definition::Class:
                type = "class"sv;
                break;
            case Definition::Casgn:
                type = "casgn"sv;
                break;
            case Definition::Alias:
                type = "alias"sv;
                break;
        }

        fmt::format_to(out,
                       "[def id={}]\n"
                       " type={}\n"
                       " defines_behavior={}\n"
                       " is_empty={}\n",
                       def.id.id(), type, (int)def.defines_behavior, (int)def.is_empty);

        if (def.defining_ref.exists()) {
            auto &ref = def.defining_ref.data(*this);
            fmt::format_to(out, " defining_ref=[{}]\n", fmt::map_join(ref.name, " ", nameToString));
        }
        if (def.parent_ref.exists()) {
            auto &ref = def.parent_ref.data(*this);
            fmt::format_to(out, " parent_ref=[{}]\n", fmt::map_join(ref.name, " ", nameToString));
        }
        if (def.aliased_ref.exists()) {
            auto &ref = def.aliased_ref.data(*this);
            fmt::format_to(out, " aliased_ref=[{}]\n", fmt::map_join(ref.name, " ", nameToString));
        }
    }
    fmt::format_to(out, "## refs:\n");
    for (auto &ref : refs) {
        vector<string> nestingStrings;
        for (auto &scope : ref.nesting) {
            auto fullScopeName = showFullName(ctx, scope);
            nestingStrings.emplace_back(fmt::format("[{}]", fmt::map_join(fullScopeName, " ", nameToString)));
        }

        auto refFullName = showFullName(ctx, ref.scope);
        fmt::format_to(out,
                       "[ref id={}]\n"
                       " scope=[{}]\n"
                       " name=[{}]\n"
                       " nesting=[{}]\n"
                       " resolved=[{}]\n"
                       " loc={}\n"
                       " is_defining_ref={}\n",

                       ref.id.id(), fmt::map_join(refFullName, " ", nameToString),
                       fmt::map_join(ref.name, " ", nameToString), fmt::join(nestingStrings, " "),
                       fmt::map_join(ref.resolved, " ", nameToString), ref.loc.filePosToString(ctx),
                       (int)ref.is_defining_ref);

        if (ref.parent_of.exists()) {
            auto parentOfFullName = showFullName(ctx, ref.parent_of);
            fmt::format_to(out, " parent_of=[{}]\n", fmt::map_join(parentOfFullName, " ", nameToString));
        }
    }
    return to_string(out);
}

class MsgpackWriter {
private:
    void packName(core::NameRef nm) {
        u4 id;
        auto it = symbolIds.find(nm);
        if (it == symbolIds.end()) {
            id = symbols.size();
            symbols.emplace_back(nm);
            symbolIds[nm] = id;
        } else {
            id = it->second;
        }
        packer.pack_uint32(id);
    }

    void packNames(vector<core::NameRef> &names) {
        packer.pack_array(names.size());
        for (auto nm : names) {
            packName(nm);
        }
    }

    void packString(string_view str) {
        packer.pack_str(str.size());
        packer.pack_str_body(str.data(), str.size());
    }

    void packString(msgpack::packer<msgpack::sbuffer> &packer, string_view str) {
        packer.pack_str(str.size());
        packer.pack_str_body(str.data(), str.size());
    }

    void packBool(bool b) {
        if (b) {
            packer.pack_true();
        } else {
            packer.pack_false();
        }
    }

    void packReferenceRef(ReferenceRef ref) {
        if (!ref.exists()) {
            packer.pack_nil();
        } else {
            packer.pack_uint16(ref.id());
        }
    }

    void packDefinitionnRef(DefinitionRef ref) {
        if (!ref.exists()) {
            packer.pack_nil();
        } else {
            packer.pack_uint16(ref.id());
        }
    }

    void packRange(u4 begin, u4 end) {
        packer.pack_uint64(((u8)begin << 32) | end);
    }

    void packDefinition(core::Context ctx, ParsedFile &pf, Definition &def) {
        packer.pack_array(def_attrs[version].size());

        // raw_full_name
        auto raw_full_name = pf.showFullName(ctx, def.id);
        packNames(raw_full_name);

        // type
        packer.pack_uint8(def.type);

        // defines_behavior
        packBool(def.defines_behavior);

        // is_empty
        packBool(def.is_empty);

        // parent_ref
        packReferenceRef(def.parent_ref);

        // aliased_ref
        packReferenceRef(def.aliased_ref);

        // defining_ref
        packReferenceRef(def.defining_ref);
    }

    void packReference(core::Context ctx, ParsedFile &pf, Reference &ref) {
        packer.pack_array(ref_attrs[version].size());

        // scope
        packDefinitionnRef(ref.scope.id());

        // name
        packNames(ref.name);

        // nesting
        packer.pack_array(ref.nesting.size());
        for (auto &scope : ref.nesting) {
            packDefinitionnRef(scope.id());
        }

        // expression_range
        auto expression_range = ref.definitionLoc.position(ctx);
        packRange(expression_range.first.line, expression_range.second.line);
        // expression_pos_range
        packRange(ref.loc.beginPos(), ref.loc.endPos());

        // resolved
        if (ref.resolved.empty()) {
            packer.pack_nil();
        } else {
            packNames(ref.resolved);
        }

        // is_defining_ref
        packBool(ref.is_defining_ref);

        // parent_of
        packDefinitionnRef(ref.parent_of);
    }

    static int assert_valid_version(int version) {
        if (version < MIN_VERSION || version > MAX_VERSION) {
            Exception::raise("msgpack version {} not in available range [{}, {}]", version, MIN_VERSION, MAX_VERSION);
        }
        return version;
    }

public:
    constexpr static int MIN_VERSION = 2;
    constexpr static int MAX_VERSION = 2;

    // symbols[0..3] are reserved for the Type aliases
    MsgpackWriter(int version)
        : version(assert_valid_version(version)), ref_attrs(ref_attr_map.at(version)),
          def_attrs(def_attr_map.at(version)), packer(payload), symbols(4) {}

    string pack(core::Context ctx, ParsedFile &pf) {
        packer.pack_array(6);

        packer.pack_true(); // did_resolution
        packString(pf.path);
        packer.pack_uint32(pf.cksum);

        // requires
        packer.pack_array(pf.requires.size());
        for (auto nm : pf.requires) {
            packString(nm.data(ctx)->show(ctx));
        }

        packer.pack_array(pf.defs.size());
        for (auto &def : pf.defs) {
            packDefinition(ctx, pf, def);
        }
        packer.pack_array(pf.refs.size());
        for (auto &ref : pf.refs) {
            packReference(ctx, pf, ref);
        }

        msgpack::sbuffer out;
        msgpack::packer<msgpack::sbuffer> header(out);
        header.pack_map(5);

        packString(header, "symbols");
        int i = -1;
        header.pack_array(symbols.size());
        for (auto sym : symbols) {
            ++i;
            string str;
            switch (i) {
                case Definition::Module:
                    str = "module";
                    break;
                case Definition::Class:
                    str = "class";
                    break;
                case Definition::Casgn:
                    str = "casgn";
                    break;
                case Definition::Alias:
                    str = "alias";
                    break;
                default:
                    str = sym.data(ctx)->show(ctx);
            }
            packString(header, str);
        }

        packString(header, "ref_count");
        header.pack_uint32(pf.refs.size());
        packString(header, "def_count");
        header.pack_uint32(pf.defs.size());

        packString(header, "ref_attrs");
        header.pack_array(ref_attrs.size());
        for (auto attr : ref_attrs) {
            packString(header, attr);
        }

        packString(header, "def_attrs");
        header.pack_array(def_attrs.size());
        for (auto attr : def_attrs) {
            packString(header, attr);
        }
        out.write(payload.data(), payload.size());
        return string(out.data(), out.size());
    }

private:
    int version;
    const vector<string> &ref_attrs;
    const vector<string> &def_attrs;
    msgpack::sbuffer payload;
    msgpack::packer<msgpack::sbuffer> packer;

    vector<core::NameRef> symbols;
    UnorderedMap<core::NameRef, u4> symbolIds;

    static const map<int, vector<string>> ref_attr_map;
    static const map<int, vector<string>> def_attr_map;
};

const map<int, vector<string>> MsgpackWriter::ref_attr_map{
    {
        2,
        {
            "scope",
            "name",
            "nesting",
            "expression_range",
            "expression_pos_range",
            "resolved",
            "is_defining_ref",
            "parent_of",
        },
    },
};

const map<int, vector<string>> MsgpackWriter::def_attr_map{
    {
        2,
        {
            "raw_full_name",
            "type",
            "defines_behavior",
            "is_empty",
            "parent_ref",
            "aliased_ref",
            "defining_ref",
        },
    },
};

string ParsedFile::toMsgpack(core::Context ctx, int version) {
    MsgpackWriter write(version);
    return write.pack(ctx, *this);
}

void ParsedFile::classlist(core::Context ctx, vector<string> &out) {
    auto nameToString = [&](const auto &nm) -> string { return nm.data(ctx)->show(ctx); };
    for (auto &def : defs) {
        if (def.type != Definition::Class) {
            continue;
        }
        auto names = showFullName(ctx, def.id);
        out.emplace_back(fmt::format("{}", fmt::map_join(names, "::", nameToString)));
    }
}

NamedDefinition ParsedFile::toNamed(core::Context ctx, DefinitionRef def) {
    auto nameToString = [&](const auto &nm) -> string { return nm.data(ctx)->show(ctx); };
    auto names = showFullName(ctx, def);
    vector<core::NameRef> parentName;
    if (def.data(*this).parent_ref.exists()) {
        auto parentRef = def.data(*this).parent_ref.data(*this);
        if (!parentRef.resolved.empty()) {
            parentName = parentRef.resolved;
        } else {
            parentName = parentRef.name;
        }
    }
    return {def.data(*this), fmt::format("{}", fmt::map_join(names, "::", nameToString)), names, parentName, requires,
            tree.file};
}

std::string_view NamedDefinition::toString(core::Context ctx) const {
    return fmt::format("DEF {} ({}) {}", name, def.type, fileRef.data(ctx).path());
}

bool AutoloaderConfig::include(core::Context ctx, const NamedDefinition &nd) const {
    return !nd.nameParts.empty() && topLevelNamespaces.find(nd.nameParts[0].show(ctx)) != topLevelNamespaces.end() &&
           includePath(nd.fileRef.data(ctx).path());
}

static const string_view requiredSuffix = ".rb";
inline bool hasRequiredSuffix(string_view path) {
    return path.size() > requiredSuffix.size() && equal(path.rbegin(), path.rend(), requiredSuffix.rbegin());
}

bool AutoloaderConfig::includePath(string_view path) const {
    if (hasRequiredSuffix(path)) {
        return false;
    }
    for (const auto &pat : excludePatterns) {
        if (regex_search(path.begin(), path.end(), pat)) {
            return false;
        }
    }
    return true;
}

bool AutoloaderConfig::includeRequire(const string &require) const {
    return excludedRequires.find(require) == excludedRequires.end();
}

void DefTree::prettyPrint(core::Context ctx, int level) {
    auto fileRefToString = [&](const NamedDefinition &nd) -> string_view { return nd.fileRef.data(ctx).path(); };
    fmt::print("{} [{}]\n", name, fmt::map_join(namedDefs, ", ", fileRefToString));
    for (auto &[name, tree] : children) {
        for (int i = 0; i < level; ++i) {
            fmt::print("  ");
        }
        tree->prettyPrint(ctx, level + 1);
    }
}

void DefTree::addDef(core::Context ctx, const AutoloaderConfig &alCfg, const NamedDefinition &ndef) {
    if (!alCfg.include(ctx, ndef)) {
        return;
    }
    auto *node = this;
    for (const auto &part : ndef.nameParts) {
        auto &child = node->children[part.show(ctx)];
        if (!child) {
            child = make_unique<DefTree>();
            child->name = part.show(ctx);

            child->nameParts = node->nameParts; // NOTE: this could be tracked recursively with push/pop
            child->nameParts.emplace_back(part);
        }
        node = &*child; // TODO yuck
    }
    if (ndef.def.defines_behavior) {
        node->namedDefs.emplace_back(ndef);
    } else {
        node->nonBehaviorDefs.emplace_back(ndef);
    }
}

static const string_view PREAMBLE = R"EOS(# frozen_string_literal: true
# DO NOT TOUCH
# This file is generated by ./scripts/bin/autogen
# Use that.
# typed: true
)EOS";

string join(string path, string file) {
    if (file.empty()) {
        return path;
    }
    return fmt::format("{}/{}", path, file);
}

void create_dir(string path) {
    auto err = mkdir(fmt::format("{}", path).c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH); // TODO cleanup
    if (err) {
        fmt::print("ERROR creating directory {}\n", path);
        throw runtime_error("create_dir() failed!");
    }
}

bool visitDefTree(const DefTree &tree, std::function<bool(const DefTree &)> visit) {
    bool descend = visit(tree);
    if (!descend) {
        return false;
    }
    for (const auto &[_, child] : tree.children) {
        descend = visitDefTree(*child, visit);
        if (!descend) {
            return false;
        }
    }
    return true;
}

core::FileRef DefTree::file() const {
    core::FileRef ref;
    if (!namedDefs.empty()) {
        // TODO what if there are more than one?
        ref = namedDefs[0].fileRef;
    } else if (!nonBehaviorDefs.empty()) {
        // TODO this lacks sorting from `definition_sort_key`
        ref = nonBehaviorDefs[0].fileRef;
    }
    return ref;
}

static const core::FileRef EMPTY_FILE;
void DefTree::prune(core::Context ctx, const AutoloaderConfig &alCfg) {
    // auto definingFile = file();
    core::FileRef definingFile = EMPTY_FILE;
    if (!namedDefs.empty()) {
        definingFile = file();
    }
    if (!prunable(ctx, alCfg)) {
        return;
    }

    // fmt::print("PRUNING FOR {} {}\n", name, definingFile.id());
    for (auto it = children.begin(); it != children.end(); ++it) {
        auto &child = it->second;
        if (child->hasDifferentFile(definingFile)) {
            child->prune(ctx, alCfg);
        } else {
            // fmt::print("  {} {}\n", child->name, child->file().id());
            children.erase(it);
        }
    }
}

bool DefTree::prunable(core::Context ctx, const AutoloaderConfig &alCfg) const {
    for (const auto &parts : alCfg.sameFileModules) {
        if (nameParts.size() != parts.size()) {
            continue;
        }
        bool match = true;
        for (int i = 0; i < parts.size(); ++i) {
            if (parts[i] != nameParts[i].show(ctx)) {
                match = false;
                break;
            }
        }
        if (match) {
            return false;
        }
    }
    return true;
}

void DefTree::merge(DefTree rhs) {
    ENFORCE(nameParts == rhs.nameParts, "Name mismatch for DefTree::merge");
    namedDefs.insert(namedDefs.end(), make_move_iterator(rhs.namedDefs.begin()),
                     make_move_iterator(rhs.namedDefs.end()));
    nonBehaviorDefs.insert(nonBehaviorDefs.end(), make_move_iterator(rhs.nonBehaviorDefs.begin()),
                           make_move_iterator(rhs.nonBehaviorDefs.end()));
    for (auto &[name, tree] : rhs.children) {
        if (children.find(name) == children.end()) {
            children[name] = move(tree);
        } else {
            children[name]->merge(move(*tree));
        }
    }
}

bool DefTree::hasDifferentFile(core::FileRef file) const {
    bool res = false;
    auto visit = [&](const DefTree &node) -> bool {
        // fmt::print("  {} {} {}\n", fileRef.id(), node.file().id(), node.name);
        auto f = node.file();
        if (file != f && f != EMPTY_FILE) {
            res = true;
            return false;
        }
        return true;
    };
    visitDefTree(*this, visit);
    return res;
}

bool DefTree::root() const {
    return nameParts.empty();
}

void DefTree::writeAutoloads(core::Context ctx, const AutoloaderConfig &alCfg, std::string path) {
    // fmt::print("writeAutoloads {} '{}'\n", name, path);
    string filename = root() ? "root.rb" : fmt::format("{}.rb", name);
    FileOps::write(join(path, filename), autoloads(ctx, alCfg));
    if (!children.empty()) {
        auto subdir = join(path, name);
        if (!name.empty()) {
            create_dir(subdir);
        }
        for (auto &[_, child] : children) {
            child->writeAutoloads(ctx, alCfg, subdir);
        }
    }
}

void DefTree::requires(core::Context ctx, const AutoloaderConfig &alCfg, fmt::memory_buffer &buf) {
    if (root() || !hasDef()) {
        return;
    }
    auto &ndef = definition();
    vector<string> reqs;
    for (auto reqRef : ndef.requires) {
        string req = reqRef.show(ctx);
        if (alCfg.includeRequire(req)) {
            reqs.emplace_back(req);
        }
    }
    fast_sort(reqs);
    auto last = unique(reqs.begin(), reqs.end());
    for (auto it = reqs.begin(); it != last; ++it) {
        fmt::format_to(buf, "require '{}'\n", *it);
    }
}

void DefTree::predeclare(core::Context ctx, string_view fullName, fmt::memory_buffer &buf) {
    if (hasDef() && definitionType() == Definition::Class) {
        // if (!namedDefs.empty() && namedDefs[0].def.type == Definition::Class) {
        fmt::format_to(buf, "\nclass {}", fullName);
        auto &def = definition();
        if (!def.parentName.empty()) {
            fmt::format_to(buf, " < {}",
                           fmt::map_join(def.parentName, "::", [&](const auto &nr) -> string { return nr.show(ctx); }));
        }
    } else {
        fmt::format_to(buf, "\nmodule {}", fullName);
    }
    // TODO aliases? casgn?
    fmt::format_to(buf, "\nend\n");
}

string DefTree::path(core::Context ctx) {
    auto toPath = [&](const auto &fr) -> string { return fr.show(ctx); };
    return fmt::format("{}.rb", fmt::map_join(nameParts, "/", toPath));
}

string DefTree::autoloads(core::Context ctx, const AutoloaderConfig &alCfg) {
    fmt::memory_buffer buf;
    fmt::format_to(buf, "{}\n", PREAMBLE);

    core::FileRef definingFile = EMPTY_FILE;
    if (!namedDefs.empty()) {
        definingFile = file();
    } else if (children.empty() && hasDef()) {
        definingFile = file();
    }
    if (definingFile != EMPTY_FILE) {
        requires(ctx, alCfg, buf);
    }

    string fullName = "nil";
    auto type = definitionType();
    if (type == Definition::Module || type == Definition::Class) {
        fullName = root() ? "Object" : fmt::format("{}", fmt::map_join(nameParts, "::", [&](const auto &nr) -> string {
                                                       return nr.show(ctx);
                                                   }));
        if (!root()) {
            fmt::format_to(buf, "Opus::Require.on_autoload('{}')\n", fullName);
            predeclare(ctx, fullName, buf);
        }
        if (!children.empty()) {
            fmt::format_to(buf, "\nOpus::Require.autoload_map({}, {{\n", fullName);
            vector<string> childNames;
            std::transform(children.begin(), children.end(), back_inserter(childNames),
                           [](const auto &pair) -> string { return pair.first; });
            fast_sort(childNames);
            for (const auto &childName : childNames) {
                fmt::format_to(buf, "  {}: \"autoloader/{}\",\n", childName, children[childName]->path(ctx));
            }
            fmt::format_to(buf, "}})\n", fullName);
        }
    }

    if (definingFile != EMPTY_FILE) {
        fmt::format_to(buf, "\nOpus::Require.for_autoload({}, \"{}\")\n", fullName, definingFile.data(ctx).path());
    }
    return to_string(buf);
}

Definition::Type DefTree::definitionType() {
    // if (namedDefs.empty()) {
    if (!hasDef()) {
        return Definition::Module;
    }
    return definition().def.type;
}

bool DefTree::hasDef() const {
    return !(namedDefs.empty() && nonBehaviorDefs.empty());
}

NamedDefinition &DefTree::definition() {
    if (!namedDefs.empty()) {
        ENFORCE(namedDefs.size() == 1, "Cannot determine definitions for '{}' (size={})", name, namedDefs.size());
        return namedDefs[0];
    } else {
        ENFORCE(!nonBehaviorDefs.empty(), "Could not find any defintions for '{}'", name);
        return nonBehaviorDefs[0];
    }
}

} // namespace sorbet::autogen