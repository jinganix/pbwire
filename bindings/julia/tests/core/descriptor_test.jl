# Stage 1 gate for the pure-Julia core: the descriptor reader over the same
# FileDescriptorSet subset the C core reads (src/descriptor_set.c).
#
# Only facts the codec depends on: field count and order, the four presence
# kinds, repetition and packedness, kind mapping, oneofs, groups and the
# transitive required closure. Every assertion comes from a `.desc` fixture the
# C tests already pin, so the two readers cannot drift.
#
#   julia --project=runtime tests/core/descriptor_test.jl [data dir]

include(joinpath(@__DIR__, "harness.jl"))

function descriptor_tests(data::AbstractString = CORE_FIXTURES)
    checks = 0
    failures = 0
    function check(ok::Bool, what::AbstractString)
        checks += 1
        if !ok
            failures += 1
            println("FAIL ", what)
        end
    end
    type_of_here(file, name) = type_of(file, name, data)

    # ------------------------------------------------------------ matrix3
    def = type_of_here("matrix3.desc", "structtest.Scalars3").def
    check(field_count(def) == 22, "matrix3.desc: 22 fields")
    check(def.full_name == "structtest.Scalars3", "matrix3.desc: full name")
    for i in 0:21
        f = def.fields[i + 1]::Field
        check(f.number == i + 1, "field $(i + 1) is numbered $(f.number)")
    end
    check((def.fields[1]::Field).name == "i32", "first field is i32")
    check((def.fields[22]::Field).name == "flags", "last field is flags")

    # proto3 plain scalars have no has-bit...
    check(field_by_name(def, "i32").presence == IMPLICIT, "i32 implicit")
    check(field_by_name(def, "str").presence == IMPLICIT, "str implicit")
    # ...while `optional` in a proto3 file is explicit presence.
    check(field_by_name(def, "opt_i32").presence == EXPLICIT, "opt_i32 explicit")
    check(field_by_name(def, "opt_str").presence == EXPLICIT, "opt_str explicit")
    # ...and a message field always has it.
    check(field_by_name(def, "inner").presence == MESSAGE_PRESENCE,
        "inner message presence")

    flags = field_by_name(def, "flags")
    check(flags.repeated, "flags repeated")
    check(flags.kind == K_BOOL, "flags is bool")
    check(flags.packed, "proto3 repeated numerics default to packed")
    check(flags.wt == 2, "a packed field encodes under the LEN tag")

    nums = field_by_name(def, "nums")
    check(nums.repeated, "nums repeated")
    check(nums.kind == K_INT32, "nums is int32")
    check(nums.packed, "nums packed")
    check(nums.elem_wt == 0, "the unpacked element form is still accepted")

    check(field_by_name(def, "i64").kind == K_INT64, "i64")
    check(field_by_name(def, "u64").kind == K_UINT64, "u64")
    check(field_by_name(def, "s64").kind == K_SINT64, "s64")
    check(field_by_name(def, "fx64").kind == K_FIXED64, "fx64")
    check(field_by_name(def, "sfx64").kind == K_SFIXED64, "sfx64")
    check(field_by_name(def, "f32").kind == K_FLOAT, "f32")
    check(field_by_name(def, "f64").kind == K_DOUBLE, "f64")
    check(field_by_name(def, "color").kind == K_ENUM, "color")
    check(field_by_name(def, "inner").kind == K_MESSAGE, "inner")
    check(field_by_name(def, "inner").msg_type.full_name == "structtest.Inner3",
        "inner type")
    # proto3 strings verify UTF-8 on parse.
    check(field_by_name(def, "str").utf8, "proto3 str verifies utf8")
    # The enum is open (proto3), and its values are indexed by name.
    color = field_by_name(def, "color").enum_type
    check(!color.closed, "proto3 enum is open")
    check(color.by_name["COLOR3_GREEN"] == 2, "COLOR3_GREEN == 2")
    check(color.by_number[2] == "COLOR3_GREEN", "enum value 2 is COLOR3_GREEN")

    # ------------------------------------------------------------ matrix2
    ty2 = type_of_here("matrix2.desc", "structtest.Scalars2")
    def2 = ty2.def
    check(field_by_name(def2, "id").presence == REQUIRED, "proto2 required")
    check(def2.has_required, "Scalars2 has a required field")
    # Inner2.name is required, so Scalars2 is required transitively too.
    check(type_of_here("matrix2.desc", "structtest.Inner2").def.has_required,
        "Inner2 has a required field")
    check(def2.deep_required, "Scalars2 is transitively required")

    # proto2 packs only with [packed = true].
    p2nums = field_by_name(def2, "nums")
    check(p2nums.repeated && !p2nums.packed, "proto2 nums is unpacked")
    check(field_by_name(def2, "packed_nums").packed, "proto2 packed_nums is packed")
    # proto2 strings do not verify UTF-8.
    check(!field_by_name(def2, "str").utf8, "proto2 str does not verify utf8")

    # ---------------------------------------------------------- grouptest
    gdef = type_of_here("grouptest.desc", "structtest.GroupTest").def
    ginner = field_by_name(gdef, "inner")
    check(ginner.kind == K_GROUP, "inner is a group")
    check(ginner.wt == 3, "start-group tag")
    check(ginner.msg_type.full_name == "structtest.GroupTest.Inner",
        "group type")
    check(ginner.tag == UInt8[0x0b], "start tag 0x0b")
    check(ginner.tag_end == UInt8[0x0c], "end tag 0x0c")
    gitem = field_by_name(gdef, "item")
    check(gitem.kind == K_GROUP, "item is a group")
    check(gitem.repeated, "item is repeated")

    # ---------------------------------------------------------- oneoftest
    odef = type_of_here("oneoftest.desc", "structtest.OneOfTest").def
    check(odef.oneof_count == 1, "one oneof")
    check(odef.oneof_by_name["kind"] == 0, "oneof kind is slot 0")
    for fname in ("num", "text", "sub", "blob")
        f = field_by_name(odef, fname)
        check(f.oneof == 0, "$fname is a member of the oneof")
        check(f.presence == EXPLICIT, "$fname has explicit presence")
    end
    check(field_by_name(odef, "id").oneof == -1, "id is not in the oneof")

    # ---------------------------------------------------------- synthetic
    node = deep_type()
    check(field_count(node.def) == 1, "synthetic: one field")
    check((node.def.fields[1]::Field).msg_type === node.def,
        "the field points back at Node")
    check(!(node.def.fields[1]::Field).utf8, "a message field carries no utf8 flag")
    check(!node.def.has_required, "proto3 message has no required field")

    return checks, failures
end

if abspath(PROGRAM_FILE) == @__FILE__
    n, f = descriptor_tests(isempty(ARGS) ? CORE_FIXTURES : ARGS[1])
    println("descriptor_test: $(n - f)/$(n) checks passed")
    exit(f == 0 ? 0 : 1)
end
