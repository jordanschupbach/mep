-- Shared machinery for the NAFEMS example set.
--
-- Every script beside this one is a benchmark: it opens a part, states
-- the benchmark's own conditions on it, solves, and compares what came
-- out against a reference. This file is what they have in common --
-- finding their own directory, picking faces and edges by where they
-- are, deriving the closed forms that several of them share, picking a
-- mode out of a list by what it *is*, and writing the film.
--
-- Run one from the repository root, either way round:
--
--     ./build/native/mep --run-lua examples/nafems/le10.lua
--     :source examples/nafems/le10.lua          (inside a running mep)
--
-- THE SCRIPTS USE ONLY THE PUBLIC SURFACE -- mep.part_* and mep.fem_*,
-- Part K.1's bindings, the same thirty-four methods an agent gets over
-- the socket. That is deliberate and it is the point of the exercise:
-- src/fem_nafems_test.cpp builds its meshes by hand in C++ and can do
-- anything the library can, so it proves the *solver*. These prove the
-- surface. Where a benchmark cannot be stated through it, the script
-- says so and stops, rather than quietly stating a different problem.

local M = {}

-- This file's own directory, so a script can find its geometry whatever
-- the working directory is. `source` is "@/path/to/nafems.lua".
local here = debug.getinfo(1, "S").source:match("^@(.*)/[^/]*$") or "examples/nafems"
M.directory = here
M.output = os.getenv("NAFEMS_OUT") or (here .. "/film")

M.pi = 3.14159265358979323846

-- --- Reporting -------------------------------------------------------
--
-- A benchmark that prints a number is a demonstration. A benchmark that
-- prints a number *next to the one it should be* is a benchmark, and the
-- percentage between them is the whole output. Everything here goes
-- through this so the set reads as one table.

M.checks = 0
M.failures = 0

function M.heading(text)
    print("")
    print(text)
    print(string.rep("-", #text))
end

function M.note(text) print("  " .. text) end

-- `tolerance` is a fraction: 0.05 means "within five percent".
function M.compare(what, got, reference, units, tolerance)
    M.checks = M.checks + 1
    local apart = reference ~= 0 and (got - reference) / math.abs(reference) or 0.0
    local verdict = math.abs(apart) <= tolerance and "ok" or "OUT"
    if verdict == "OUT" then M.failures = M.failures + 1 end
    print(string.format("  %-34s %14.6g %-8s reference %14.6g  %+7.3f%%  %s",
                        what, got, units, reference, apart * 100.0, verdict))
    return math.abs(apart) <= tolerance
end

-- The same, but against an absolute tolerance. For a quantity that
-- passes through zero -- the radial stress at the outside of a
-- pressurised cylinder is meant to be nothing -- a percentage is
-- meaningless and a fixed band is what the comparison actually means.
function M.close(what, got, reference, units, tolerance)
    M.checks = M.checks + 1
    local apart = math.abs(got - reference)
    local verdict = apart <= tolerance and "ok" or "OUT"
    if verdict == "OUT" then M.failures = M.failures + 1 end
    print(string.format("  %-34s %14.6g %-8s reference %14.6g  %8.4g off %s",
                        what, got, units, reference, apart, verdict))
    return apart <= tolerance
end

function M.verdict(name)
    print("")
    if M.failures == 0 then
        print(string.format("%s: %d of %d comparisons within tolerance", name, M.checks, M.checks))
        return 0
    end
    print(string.format("%s: %d of %d comparisons OUT OF TOLERANCE", name, M.failures, M.checks))
    return 1
end

-- --- Geometry --------------------------------------------------------

function M.open(id)
    local document, err = mep.part_import{path = here .. "/" .. id .. ".mepcad"}
    if not document then error(id .. ".mepcad would not open: " .. tostring(err)) end
    return document.document, 0, document
end

local function near(a, b, tolerance) return math.abs(a - b) <= (tolerance or 1e-6) end

-- Every face of the body, each with its own bounding box.
--
-- PICK FACES BY THEIR BOX, NOT BY THEIR SAMPLE POINT. part.faces reports
-- a `point` that is the *surface* evaluated at the middle of its own
-- parameter domain, and a face is a trimmed piece of a surface: the
-- outer elliptical face of LE10's quarter plate reports a point at
-- (-3.25, 0, 0), which is on the opposite side of the whole ellipse and
-- outside the part. Picking "the face nearest (3.25, 0, 0)" therefore
-- returns the face bounding the *hole*, the benchmark is built in along
-- the wrong edge, and the answer is wrong by a factor of three with
-- nothing having failed. `low` and `high` come from the face's own
-- tessellation and say where it really is.
function M.faces(document, body)
    return mep.part_faces{document = document, body = body}.faces
end

-- The faces satisfying a predicate, highest score first when the
-- predicate returns one.
function M.face_where(document, body, predicate)
    local found = {}
    for _, face in ipairs(M.faces(document, body)) do
        if predicate(face) then found[#found + 1] = face end
    end
    return found
end

function M.one_face(document, body, predicate, description)
    local found = M.face_where(document, body, predicate)
    if #found == 0 then error("no face is " .. (description or "what was asked for")) end
    if #found > 1 then
        error(string.format("%d faces are %s; the description does not name one", #found,
                            description or "what was asked for"))
    end
    return found[1].id, found[1]
end

-- A coordinate plane of the body: the faces that are flat in `axis` at
-- `value`. A symmetry plane can be more than one face once a boolean has
-- been through it, so this returns a list and the caller holds all of
-- them.
function M.plane(document, body, axis, value, tolerance)
    local index = ({x = 1, y = 2, z = 3})[axis]
    local ids = {}
    for _, face in ipairs(M.faces(document, body)) do
        if near(face.low[index], value, tolerance) and near(face.high[index], value, tolerance) then
            ids[#ids + 1] = face.id
        end
    end
    if #ids == 0 then error("nothing lies in the plane " .. axis .. " = " .. tostring(value)) end
    return ids
end

-- Holds one component on a whole coordinate plane: the symmetry
-- condition, which is the only support most of these benchmarks have.
function M.symmetry(study, document, body, axis, value)
    local fixed = {axis == "x", axis == "y", axis == "z"}
    local ids = M.plane(document, body, axis, value)
    for _, id in ipairs(ids) do
        mep.fem_support{study = study, face = id, fixed = fixed,
                        label = axis .. " = " .. tostring(value)}
    end
    return #ids
end

function M.edge_near(document, body, point, along)
    local found = mep.part_edge_at{document = document, body = body, point = point, along = along}
    if not found then error("no edge near that point") end
    return found.edge, found
end

-- A usable mesh, trying each method and size in turn, and saying what
-- was refused.
--
-- NOT A CONVENIENCE. mep's meshers refuse several of these parts, and
-- the refusals are not random: the surface mesher produces slivers on a
-- planar face that is not a rectangle -- a rhombus or a trapezoid is
-- enough -- and the sweeper inherits them. Asking for one size and one
-- method and stopping would make an example that fails for a reason
-- with nothing to do with the benchmark. Asking for a list, and printing
-- what was refused, makes the limitation part of the output instead of
-- an obstacle to it.
--
-- SWEEP FIRST, WHERE THE PART ALLOWS IT. Every part in this set except
-- the sphere is a profile extruded along a line, and Hex8 against Tet4
-- in bending is not a close contest: four nodes cannot represent a
-- strain that varies across the element, so a tetrahedral plate comes
-- out far too stiff. Part G.6's sweeper gives hexahedra for exactly this
-- shape of part, and until these examples needed it, it was reachable
-- only from C++.
function M.mesh(document, body, sizes, order, methods)
    methods = methods or {"sweep", "tetrahedra"}
    local refused = {}
    for _, how in ipairs(methods) do
        for _, size in ipairs(sizes) do
            local mesh, err = mep.fem_mesh{document = document, body = body, size = size,
                                           order = order or 1, method = how}
            if mesh then
                for _, line in ipairs(refused) do M.note("  " .. line) end
                M.note(string.format(
                    "mesh  %s at %.3f m: %d nodes, %d %s, worst dihedral %.1f degrees",
                    how, size, mesh.nodes, mesh.elements, mesh.shape, mesh.worst_dihedral))
                mesh.size = size
                mesh.method = how
                return mesh
            end
            refused[#refused + 1] = string.format("%s at %.3f refused: %s", how, size,
                                                  tostring(err):sub(1, 80))
        end
    end
    for _, line in ipairs(refused) do M.note("  " .. line) end
    return nil
end

-- What a script says when the mesher will not give it a mesh. Not an
-- error: the benchmark is stated correctly, the part is valid -- it is
-- mep's mesher that cannot yet take it, and saying so plainly is more
-- use than a stack trace.
function M.blocked(name, why, missing)
    print("")
    print(string.format("%s: BLOCKED -- %s", name, why))
    print("  The benchmark's own conditions are stated above and are correct;")
    print(string.format("  what is missing is %s.", missing or "a mesh"))
    print("  See plans/NAFEMS_PLAN.md, 'The examples, and what they found'.")
    return 2
end

-- --- Closed forms several of the benchmarks share --------------------

-- Lame's thick cylinder: the exact stress anywhere in the wall of a tube
-- under internal pressure. Exact rather than asymptotic, which is what
-- makes LE7 checkable at every node instead of at one.
function M.lame(pressure, a, b, r)
    local common = pressure * a * a / (b * b - a * a)
    return {
        radial = common * (1.0 - b * b / (r * r)),
        hoop   = common * (1.0 + b * b / (r * r)),
        axial  = common,  -- closed ends
    }
end

-- Timoshenko's simply-supported beam: the lower root of
--   w^4 - p w^2 + q = 0, with the coefficients below. The upper root is
-- the shear mode, far above. As G grows the third term of p dominates
-- and this tends to Euler-Bernoulli, which is a check on the algebra
-- rather than a separate formula.
function M.timoshenko(n, span, depth, width, modulus, poisson, density)
    local area = depth * width
    local inertia = width * depth ^ 3 / 12.0
    local shear_modulus = modulus / (2.0 * (1.0 + poisson))
    local kappa = 10.0 * (1.0 + poisson) / (12.0 + 11.0 * poisson)  -- rectangle
    local beta = n * M.pi / span
    local p = kappa * shear_modulus * beta * beta / density
            + modulus * beta * beta / density
            + kappa * area * shear_modulus / (density * inertia)
    local q = kappa * shear_modulus * modulus * beta ^ 4 / (density * density)
    local omega2 = 0.5 * (p - math.sqrt(p * p - 4.0 * q))
    return math.sqrt(omega2) / (2.0 * M.pi)
end

-- Mindlin's thick plate: Kirchhoff softened by shear deformation and by
-- rotary inertia, the two things a thin-plate theory leaves out and a
-- plate a tenth of its span thick very much has.
function M.mindlin(m, n, side, thickness, modulus, poisson, density)
    local flexural = modulus * thickness ^ 3 / (12.0 * (1.0 - poisson * poisson))
    local beta2 = (m * M.pi / side) ^ 2 + (n * M.pi / side) ^ 2
    local kirchhoff2 = flexural * beta2 * beta2 / (density * thickness)
    local shear_modulus = modulus / (2.0 * (1.0 + poisson))
    local kappa = 5.0 / 6.0
    local shear = flexural * beta2 / (kappa * shear_modulus * thickness)
    local rotary = thickness * thickness * beta2 / 12.0
    return math.sqrt(kirchhoff2 / (1.0 + shear + rotary)) / (2.0 * M.pi)
end

-- --- Picking a mode out of a list by what it is ----------------------
--
-- THE HARD PART OF EVERY MODAL BENCHMARK, and the reason fem.nodes
-- exists. A frequency on its own says nothing: the mode the benchmark
-- names may be third in one mesh and sixth in another, and a script that
-- takes "the first" or "the one nearest the answer" is not measuring
-- anything. So the shape is fetched and scored.
--
-- `score` is called with a table of {x, y, z, ux, uy, uz} per node and
-- returns a number; the highest-scoring mode wins.
function M.pick_mode(result, count, score)
    local coordinates = mep.fem_nodes{result = result}
    local best, best_score, scores = nil, nil, {}
    for mode = 0, count - 1 do
        local animation = mep.fem_animate{result = result, mode = mode, frames = 1}
        local shape = animation.displacement[1]
        local nodes = {}
        for n = 1, coordinates.count do
            nodes[n] = {
                x = coordinates.nodes[(n - 1) * 3 + 1],
                y = coordinates.nodes[(n - 1) * 3 + 2],
                z = coordinates.nodes[(n - 1) * 3 + 3],
                ux = shape[(n - 1) * 3 + 1],
                uy = shape[(n - 1) * 3 + 2],
                uz = shape[(n - 1) * 3 + 3],
            }
        end
        scores[mode + 1] = score(nodes)
        if best_score == nil or scores[mode + 1] > best_score then
            best, best_score = mode, scores[mode + 1]
        end
    end
    return best, best_score, scores
end

-- --- Film ------------------------------------------------------------

local function ensure_output()
    -- No mkdir on the Lua surface, and none needed: os.execute is here
    -- for exactly this and the directory is the script's own.
    os.execute("mkdir -p '" .. M.output .. "'")
end

function M.film(result, id, options)
    ensure_output()
    options = options or {}
    options.result = result
    options.path = M.output .. "/" .. id .. ".mov"
    local made, err = mep.fem_movie(options)
    if not made then
        M.note("the film could not be written: " .. tostring(err))
        return nil
    end
    M.note(string.format("film  %s  %d frames, %.1f s, %d kB", made.path, made.frames,
                         made.seconds, math.floor(made.bytes / 1024)))
    return made
end

function M.still(result, id, options)
    ensure_output()
    options = options or {}
    options.result = result
    options.path = M.output .. "/" .. id .. ".png"
    local made, err = mep.fem_image(options)
    if not made then
        M.note("the still could not be written: " .. tostring(err))
        return nil
    end
    M.note(string.format("still %s  %d kB", made.path, math.floor(made.bytes / 1024)))
    return made
end

return M
