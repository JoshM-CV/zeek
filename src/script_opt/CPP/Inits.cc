// See the file "COPYING" in the main distribution directory for copyright.

#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

#include "zeek/module_util.h"
#include "zeek/script_opt/CPP/Compile.h"
#include "zeek/script_opt/IDOptInfo.h"
#include "zeek/script_opt/ProfileFunc.h"

namespace zeek::detail
	{

using namespace std;

std::shared_ptr<CPP_GlobalInfo> CPPCompile::GenInitExpr(const ExprPtr& e)
	{
	NL();

	const auto& t = e->GetType();
	auto ename = InitExprName(e);

	// First, create a CPPFunc that we can compile to compute 'e'.
	auto name = string("wrapper_") + ename;

	// Forward declaration of the function that computes 'e'.
	Emit("static %s %s(Frame* f__CPP);", FullTypeName(t), name);

	// Create the Func subclass that can be used in a CallExpr to
	// evaluate 'e'.
	Emit("class %s_cl : public CPPFunc", name);
	StartBlock();

	Emit("public:");
	Emit("%s_cl() : CPPFunc(\"%s\", %s)", name, name, e->IsPure() ? "true" : "false");

	StartBlock();
	Emit("type = make_intrusive<FuncType>(make_intrusive<RecordType>(new type_decl_list()), %s, "
	     "FUNC_FLAVOR_FUNCTION);",
	     GenTypeName(t));

	EndBlock();

	Emit("ValPtr Invoke(zeek::Args* args, Frame* parent) const override final");
	StartBlock();

	if ( IsNativeType(t) )
		GenInvokeBody(name, t, "parent");
	else
		Emit("return %s(parent);", name);

	EndBlock();
	EndBlock(true);

	// Now the implementation of computing 'e'.
	Emit("static %s %s(Frame* f__CPP)", FullTypeName(t), name);
	StartBlock();

	Emit("return %s;", GenExpr(e, GEN_NATIVE));
	EndBlock();

	Emit("CallExprPtr %s;", ename);

	auto wrapper_cl = string("wrapper_") + name + "_cl";

	auto gi = make_shared<CallExprInitInfo>(this, ename, wrapper_cl, t);
	call_exprs_info->AddInstance(gi);

	return gi;
	}

bool CPPCompile::IsSimpleInitExpr(const ExprPtr& e)
	{
	switch ( e->Tag() )
		{
		case EXPR_CONST:
		case EXPR_NAME:
			return true;

		case EXPR_RECORD_COERCE:
				{ // look for coercion of empty record
				auto op = e->GetOp1();

				if ( op->Tag() != EXPR_RECORD_CONSTRUCTOR )
					return false;

				auto rc = static_cast<const RecordConstructorExpr*>(op.get());
				const auto& exprs = rc->Op()->AsListExpr()->Exprs();

				return exprs.length() == 0;
				}

		default:
			return false;
		}
	}

string CPPCompile::InitExprName(const ExprPtr& e)
	{
	return init_exprs.KeyName(e);
	}

void CPPCompile::InitializeFieldMappings()
	{
	Emit("std::vector<CPP_FieldMapping> CPP__field_mappings__ = ");

	StartBlock();

	for ( const auto& mapping : field_decls )
		{
		auto rt_arg = Fmt(mapping.first);
		auto td = mapping.second;
		auto type_arg = Fmt(TypeOffset(td->type));
		auto attrs_arg = Fmt(AttributesOffset(td->attrs));

		Emit("CPP_FieldMapping(%s, \"%s\", %s, %s),", rt_arg, td->id, type_arg, attrs_arg);
		}

	EndBlock(true);
	}

void CPPCompile::InitializeEnumMappings()
	{
	Emit("std::vector<CPP_EnumMapping> CPP__enum_mappings__ = ");

	StartBlock();

	for ( const auto& mapping : enum_names )
		Emit("CPP_EnumMapping(%s, \"%s\"),", Fmt(mapping.first), mapping.second);

	EndBlock(true);
	}

void CPPCompile::InitializeBiFs()
	{
	Emit("std::vector<CPP_LookupBiF> CPP__BiF_lookups__ = ");

	StartBlock();

	for ( const auto& b : BiFs )
		Emit("CPP_LookupBiF(%s, \"%s\"),", b.first, b.second);

	EndBlock(true);
	}

void CPPCompile::InitializeGlobals()
	{
	Emit("std::vector<CPP_GlobalInit> CPP__global_inits__ = ");

	StartBlock();

	for ( const auto& gi : global_gis )
		Emit("%s,", gi.second->Initializer());

	EndBlock(true);
	}

void CPPCompile::GenInitHook()
	{
	NL();

	Emit("int hook_in_init()");

	StartBlock();

	Emit("CPP_init_funcs.push_back(init__CPP);");

	if ( standalone )
		GenLoad();

	Emit("return 0;");
	EndBlock();

	// Trigger the activation of the hook at run-time.
	NL();
	Emit("static int dummy = hook_in_init();\n");
	}

void CPPCompile::GenStandaloneActivation()
	{
	NL();

#if 0
	Emit("void standalone_activation__CPP()");
	StartBlock();
	for ( auto& a : activations )
		Emit(a);
	EndBlock();
#endif

	NL();
	Emit("void standalone_init__CPP()");
	StartBlock();

	// For events and hooks, we need to add each compiled body *unless*
	// it's already there (which could be the case if the standalone
	// code wasn't run standalone but instead with the original scripts).
	// For events, we also register them in order to activate the
	// associated scripts.

	// First, build up a list of per-hook/event handler bodies.
	unordered_map<const Func*, vector<p_hash_type>> func_bodies;

	for ( const auto& func : funcs )
		{
		auto f = func.Func();
		auto fname = BodyName(func);
		auto bname = Canonicalize(fname.c_str()) + "_zf";

		if ( compiled_funcs.count(bname) == 0 )
			// We didn't wind up compiling it.
			continue;

		ASSERT(body_hashes.count(bname) > 0);
		func_bodies[f].push_back(body_hashes[bname]);
		}

	for ( auto& fb : func_bodies )
		{
		string hashes;
		for ( auto h : fb.second )
			{
			if ( hashes.size() > 0 )
				hashes += ", ";

			hashes += Fmt(h);
			}

		hashes = "{" + hashes + "}";

		auto f = fb.first;
		auto fn = f->Name();
		const auto& ft = f->GetType();

		auto var = extract_var_name(fn);
		auto mod = extract_module_name(fn);
		module_names.insert(mod);

		auto fid = lookup_ID(var.c_str(), mod.c_str(), false, true, false);
		if ( ! fid )
			reporter->InternalError("can't find identifier %s", fn);

		auto exported = fid->IsExport() ? "true" : "false";

		Emit("activate_bodies__CPP(\"%s\", \"%s\", %s, %s, %s);", var, mod, exported,
		     GenTypeName(ft), hashes);
		}

	NL();
	Emit("CPP_activation_funcs.push_back(standalone_activation__CPP);");
	Emit("CPP_activation_hook = activate__CPPs;");

	EndBlock();
	}

void CPPCompile::GenLoad()
	{
	// First, generate a hash unique to this compilation.
	auto t = util::current_time();
	auto th = hash<double>{}(t);

	total_hash = merge_p_hashes(total_hash, th);

	Emit("register_scripts__CPP(%s, standalone_init__CPP);", Fmt(total_hash));

	// Spit out the placeholder script, and any associated module
	// definitions.
	for ( const auto& m : module_names )
		if ( m != "GLOBAL" )
			printf("module %s;\n", m.c_str());

	if ( module_names.size() > 0 )
		printf("module GLOBAL;\n\n");

	printf("global init_CPP_%llu = load_CPP(%llu);\n", total_hash, total_hash);
	}

	} // zeek::detail
