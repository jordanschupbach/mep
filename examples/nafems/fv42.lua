-- NAFEMS FV42 -- thick hollow sphere, free radial vibration.
--
-- Inner radius 1 m, outer 2 m, free, E = 200 GPa, v = 0.3,
-- rho = 8000 kg/m3.
--
-- A HOLLOW SPHERE CANNOT BE MADE THROUGH THIS SURFACE AT ALL, which is
-- the finding this example exists to record. Three routes, three
-- refusals, each of them a documented limitation rather than a bug:
--
--   * as a **revolve**, the profile -- the region between two circles in
--     a half-plane -- necessarily touches the axis at both poles, and
--     the revolve refuses a profile that touches its axis by name. So
--     there is no feature tree and no .mepcad file;
--   * as a **difference of two spheres**, cad_boolean refuses it: "the
--     first solid has a face whose parameterization has a pole (a
--     sphere's poles, a cone's apex, a revolution meeting its axis);
--     cutting such a face is not implemented";
--   * as a **shell**, part.shell handles planar faces only: "face 0 is
--     not a plane".
--
-- The reference below is derived anyway, exactly and from first
-- principles, because it is the part of this benchmark that does not
-- depend on the kernel -- and because src/fem_nafems_test.cpp does run
-- FV42, on a cubed-sphere mesh built in C++, and lands on this number.
-- What is missing is a way to *model* the part, not a way to solve it.
--
-- THE REFERENCE IS EXACT AND DERIVED HERE. Purely radial motion of a
-- sphere satisfies the spherical Bessel equation of order one, so the
-- displacement is a combination of j1(kr) and y1(kr) with k = omega / c
-- and c the dilatational wave speed. Both surfaces are traction free,
-- which makes a 2 x 2 determinant, and its roots are the frequencies.
-- No table, no remembered number: the transcendental equation is solved
-- below by scanning for a sign change and bisecting.

local nafems = dofile((debug.getinfo(1, "S").source:match("^@(.*)/[^/]*$") or ".") .. "/nafems.lua")

nafems.heading("NAFEMS FV42 -- thick hollow sphere")

local inner, outer = 1.0, 2.0
local modulus, poisson, density = 200e9, 0.3, 8000.0

local lame = modulus * poisson / ((1.0 + poisson) * (1.0 - 2.0 * poisson))
local shear = modulus / (2.0 * (1.0 + poisson))
local speed = math.sqrt((lame + 2.0 * shear) / density)

local function j1(x) return math.sin(x) / (x * x) - math.cos(x) / x end
local function y1(x) return -math.cos(x) / (x * x) - math.sin(x) / x end
-- From the recurrence f1' = f0 - 2 f1 / x, with j0 = sin x / x and
-- y0 = -cos x / x.
local function dj1(x) return math.sin(x) / x - 2.0 * j1(x) / x end
local function dy1(x) return -math.cos(x) / x - 2.0 * y1(x) / x end

-- The radial traction a solution contributes at radius r:
-- sigma_rr = (lambda + 2 mu) U' + 2 lambda U / r.
local function traction(f, df, k, r)
    return (lame + 2.0 * shear) * k * df(k * r) + 2.0 * lame * f(k * r) / r
end
local function determinant(omega)
    local k = omega / speed
    return traction(j1, dj1, k, inner) * traction(y1, dy1, k, outer)
         - traction(j1, dj1, k, outer) * traction(y1, dy1, k, inner)
end
local function exact_frequencies(how_many)
    local out = {}
    local previous = determinant(1.0)
    local omega = 1.0
    while omega < 60000.0 and #out < how_many do
        local current = determinant(omega + 1.0)
        if previous * current < 0.0 then
            local low, high = omega, omega + 1.0
            for _ = 1, 200 do
                local middle = 0.5 * (low + high)
                if determinant(low) * determinant(middle) <= 0.0 then high = middle
                else low = middle end
            end
            out[#out + 1] = 0.5 * (low + high) / (2.0 * nafems.pi)
        end
        previous = current
        omega = omega + 1.0
    end
    return out
end

local exact = exact_frequencies(3)
nafems.note(string.format("the traction-free determinant's first roots: %.4f, %.4f, %.4f Hz",
                          exact[1], exact[2], exact[3]))

-- The part. Every route is tried, and every refusal is printed: a
-- limitation stated in the output is more use than one discovered again
-- next time.
local document = mep.part_new{title = "NAFEMS FV42"}.document
local big = mep.part_sphere{document = document, radius = outer}.body
local small = mep.part_sphere{document = document, radius = inner}.body
local shell, boolean_error = mep.part_boolean{document = document, op = "difference",
                                              a = big, b = small}
if not shell then
    nafems.note("difference of two spheres: " .. tostring(boolean_error))
end
local hollowed, shell_error = mep.part_shell{document = document, body = big, thickness = 1.0}
if not hollowed then
    nafems.note("shelling the outer sphere: " .. tostring(shell_error))
end

local body = shell and shell.body or (hollowed and hollowed.body)
if not body then
    os.exit(nafems.blocked("FV42", "the kernel offers no way to make a hollow sphere",
                           "a way to model the part -- the solver is not the problem"))
end

local mass = mep.part_mass{document = document, body = body}
local exact_volume = (4.0 / 3.0) * nafems.pi * (outer ^ 3 - inner ^ 3)
nafems.compare("the shell's volume", mass.volume, exact_volume, "m3", 0.02)

local step = mep.part_export{document = document, body = body,
                             path = nafems.directory .. "/fv42.step"}
if step then nafems.note("written to " .. step.path .. " for the CAD pane") end

local mesh = nafems.mesh(document, body, {0.35, 0.5, 0.7}, 1, {"tetrahedra"})
if not mesh then os.exit(nafems.blocked("FV42", "no mesher would take the hollow sphere")) end

local study = mep.fem_study{document = document, name = "FV42"}.study
mep.fem_material{study = study, youngs_modulus = modulus, poissons_ratio = poisson,
                 density = density}

local modal = mep.fem_modal{study = study, mesh = mesh.mesh, modes = 10}
nafems.note(string.format("%d modes, of which %d are rigid-body motions",
                          #modal.frequencies, modal.rigid_body_modes))

-- The breathing mode is the one whose motion is radial everywhere. On a
-- sphere that is enough on its own -- unlike the cylinder of FV41, which
-- needed a second discriminator because a short cylinder has a cluster
-- of nearly-radial modes and a sphere does not.
local breathing, best, all = nafems.pick_mode(modal.result, #modal.frequencies, function(nodes)
    local radial, total = 0.0, 0.0
    for _, node in ipairs(nodes) do
        local r = math.sqrt(node.x ^ 2 + node.y ^ 2 + node.z ^ 2)
        local u_r = r > 1e-9 and (node.ux * node.x + node.uy * node.y + node.uz * node.z) / r or 0
        radial = radial + u_r * u_r
        total = total + node.ux ^ 2 + node.uy ^ 2 + node.uz ^ 2
    end
    return total > 0.0 and radial / total or 0.0
end)
nafems.note("mode / frequency / how radial:")
for mode = 0, #modal.frequencies - 1 do
    nafems.note(string.format("    %2d  %9.3f Hz   %.3f%s", mode + 1, modal.frequencies[mode + 1],
                              all[mode + 1], mode == breathing and "   <- breathing" or ""))
end
nafems.compare("the first radial mode", modal.frequencies[breathing + 1], exact[1], "Hz", 0.05)
nafems.note(string.format("its motion is %.3f radial", best))

nafems.still(modal.result, "fv42", {mode = breathing, caption = "FV42  RADIAL MODE",
                                    yaw = 35, pitch = 30})
nafems.film(modal.result, "fv42", {mode = breathing, frames = 30,
                                   caption = "FV42  THICK HOLLOW SPHERE", yaw = 35, pitch = 30})

os.exit(nafems.verdict("FV42"))
