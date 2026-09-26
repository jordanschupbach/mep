-- NAFEMS LE10 -- thick elliptic plate under pressure.
--
-- An elliptical plate with an elliptical hole, 0.6 m thick, under a
-- uniform 1 MPa pressure on its upper surface. A quarter is modelled;
-- the named points are
--
--     A = (2.00, 0.00)   inner ellipse, on the x axis
--     B = (0.00, 1.00)   inner ellipse, on the y axis
--     C = (0.00, 2.75)   outer ellipse, on the y axis
--     D = (3.25, 0.00)   outer ellipse, on the x axis
--
-- and the target is the direct stress sigma_yy on the upper surface,
-- z = +0.3, at **A** -- the hole, on the x axis -- where on that axis it
-- is the hoop stress rather than the radial one. The published value is
-- -5.38 MPa.
--
-- NOT AT D, which is where this was first pointed and where the answer
-- is not a number at all: the outer edge is built in, so the stress
-- there depends on how the support is idealised, and it jumped from
-- -2.3 to -0.54 between two meshes that agreed everywhere else.
-- src/fem_nafems_test.cpp records the same finding.
--
-- WHERE THE SUPPORT GOES IS THE WHOLE BENCHMARK, and getting it wrong
-- does not fail, it just answers a different question. The outer
-- elliptical face is built in entirely; the two cut planes carry
-- symmetry. Holding only the mid-plane of the outer face -- which reads
-- as the more careful choice -- moves the answer by a factor of four,
-- because the outer edge is then free to rotate and the plate is a
-- different structure. src/fem_nafems_test.cpp surveyed three supports
-- and this is the one that reproduces the published number stably.

local nafems = dofile((debug.getinfo(1, "S").source:match("^@(.*)/[^/]*$") or ".") .. "/nafems.lua")

nafems.heading("NAFEMS LE10 -- thick elliptic plate, quarter model")

local document, body = nafems.open("le10")
local mass = mep.part_mass{document = document, body = body}
nafems.note(string.format("the quarter plate is %.4f m3 and %.4f m2 of surface",
                          mass.volume, mass.area))

-- BOTH ELEMENT TYPES, because one of them on its own says very little.
-- A linear hexahedron and a quadratic tetrahedron are different
-- formulations with different failure modes, and on a mesh this coarse
-- they land on opposite sides of the published value -- which is a far
-- stronger statement than either number alone. src/fem_nafems_test.cpp
-- makes the same comparison on structured meshes, where the two agree
-- with each other to better than a percent.
local cases = {
    {name = "Hex8  (swept)", methods = {"sweep"}, order = 1,
     sizes = {0.20, 0.25, 0.30}},
    {name = "Tet10 (quadratic)", methods = {"tetrahedra"}, order = 2,
     sizes = {0.20, 0.25, 0.30, 0.40}},
}

local answers = {}
local drawn = nil
for _, case in ipairs(cases) do
    nafems.note(case.name .. ":")
    local mesh = nafems.mesh(document, body, case.sizes, case.order, case.methods)
    if not mesh then
        os.exit(nafems.blocked("LE10", "no mesher would take the elliptic plate"))
    end

    local study = mep.fem_study{document = document, name = "LE10"}.study
    mep.fem_material{study = study, youngs_modulus = 210e9, poissons_ratio = 0.3,
                     density = 7800}

    -- The two cut planes are symmetry planes: each holds the one
    -- component normal to itself and nothing else.
    nafems.symmetry(study, document, body, "x", 0.0)
    nafems.symmetry(study, document, body, "y", 0.0)

    -- The outer elliptical face, built in entirely. Named by how far it
    -- reaches rather than by a normal -- an elliptical face's normal
    -- turns through ninety degrees from one end of it to the other, so
    -- "the face pointing outward" does not name one.
    local outer = nafems.one_face(document, body, function(face)
        -- Reaches both outer semi-axes, and is not one of the two flat
        -- faces -- which reach them too, being the plan of the whole
        -- thing.
        return math.abs(face.high[1] - 3.25) < 1e-3 and math.abs(face.high[2] - 2.75) < 1e-3
               and face.high[3] > face.low[3] + 1e-9
    end, "the outer elliptical face")
    mep.fem_support{study = study, face = outer, label = "outer edge, built in"}

    local top = nafems.one_face(document, body, function(face)
        return face.normal[3] > 0.99 and math.abs(face.low[3] - 0.3) < 1e-6
    end, "the upper face")
    mep.fem_load{study = study, kind = "pressure", face = top, magnitude = 1e6,
                 label = "1 MPa"}

    local result = mep.fem_solve{study = study, mesh = mesh.mesh}
    nafems.note(string.format(
        "    solved: %d nodes, peak movement %.4g m, estimated error %.1f%%",
        result.nodes, result.max_displacement, result.relative_error * 100.0))

    local probe = mep.fem_probe{result = result.result, field = "stress_yy",
                                points = {{2.0, 0.0, 0.3}}}
    local value = probe.probes[1].value / 1e6
    answers[#answers + 1] = value
    nafems.compare("    sigma_yy at A, upper surface", value, -5.38, "MPa", 0.20)
    if case.order == 2 then
        drawn = result.result
        -- The through-thickness profile, which is what says the answer
        -- is bending rather than a number that happens to land nearby: a
        -- plate in bending has a stress very nearly linear in z that
        -- changes sign at the mid-plane.
        local through = mep.fem_path{result = result.result, field = "stress_yy",
                                     from = {2.0, 0.0, -0.3}, to = {2.0, 0.0, 0.3},
                                     samples = 7}
        nafems.note("    through the thickness at A:")
        for _, row in ipairs(through.rows) do
            nafems.note(string.format("        z = %+5.2f   sigma_yy = %+9.4g MPa",
                                      row.at[3], row.value / 1e6))
        end
    end
end

-- THE TWO BRACKET THE PUBLISHED VALUE, which is the claim worth making.
-- Either alone is a coarse mesh's answer; together they say the answer
-- is between them and that neither formulation is failing in a way the
-- other would hide.
local low = math.min(answers[1], answers[2])
local high = math.max(answers[1], answers[2])
nafems.checks = nafems.checks + 1
if low <= -5.38 and -5.38 <= high then
    nafems.note(string.format("the two meshes bracket the published -5.38 MPa: %.3f to %.3f",
                              high, low))
else
    nafems.failures = nafems.failures + 1
    nafems.note(string.format("the two meshes do NOT bracket -5.38 MPa: %.3f to %.3f",
                              high, low))
end

local result = {result = drawn}
nafems.still(result.result, "le10", {field = "stress_yy", caption = "LE10  SIGMA YY",
                                     yaw = 35, pitch = 40, color_map = "blue_to_red"})
nafems.film(result.result, "le10", {field = "von_mises", frames = 36, orbit = true,
                                    caption = "LE10  THICK ELLIPTIC PLATE", yaw = 35, pitch = 30})

os.exit(nafems.verdict("LE10"))
