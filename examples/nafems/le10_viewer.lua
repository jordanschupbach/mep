-- A viewer for NAFEMS LE10, driven by time.
--
-- Open it beside a viewer pane and edit it: `:w` rebuilds the scene.
--
--     :Viewer examples/nafems/le10_viewer.lua
--     :vsplit examples/nafems/le10_viewer.lua
--
-- Drag in the viewport to turn the part, shift-drag or middle-drag to
-- pan, the wheel to zoom, and drag the slider along the bottom to move
-- through the load cycle. `p` plays it.
--
-- WHAT THE TIME MEANS HERE. A linear static result scales exactly with
-- its load, so every frame of this is a real answer to a real load case
-- rather than an interpolation between two: at t = 0.5 the plate is
-- genuinely carrying half a megapascal. The same is not true of a mode
-- animation, where the time is a phase and the amplitude is chosen from
-- the geometry -- see le10_viewer's modal cousin in the plan.

local document = mep.part_import{path = "examples/nafems/le10.mepcad"}
if not document then error("le10.mepcad would not open") end

local mesh = mep.fem_mesh{document = document.document, body = 0, size = 0.25,
                          method = "sweep"}
if not mesh then error("the plate would not mesh") end

local study = mep.fem_study{document = document.document, name = "LE10"}.study
mep.fem_material{study = study, youngs_modulus = 210e9, poissons_ratio = 0.3,
                 density = 7800}

-- The benchmark's own conditions: symmetry on the two cut planes, the
-- outer elliptical face built in, 1 MPa on top.
for _, face in ipairs(mep.part_faces{document = document.document, body = 0}.faces) do
    local flat_in = function(axis)
        return math.abs(face.low[axis]) < 1e-6 and math.abs(face.high[axis]) < 1e-6
    end
    if flat_in(1) then
        mep.fem_support{study = study, face = face.id, fixed = {true, false, false}}
    elseif flat_in(2) then
        mep.fem_support{study = study, face = face.id, fixed = {false, true, false}}
    elseif math.abs(face.high[1] - 3.25) < 1e-3 and math.abs(face.high[2] - 2.75) < 1e-3
           and face.high[3] > face.low[3] + 1e-9 then
        mep.fem_support{study = study, face = face.id}
    elseif face.normal[3] > 0.99 and math.abs(face.low[3] - 0.3) < 1e-6 then
        mep.fem_load{study = study, kind = "pressure", face = face.id, magnitude = 1e6}
    end
end

local solved = mep.fem_solve{study = study, mesh = mesh.mesh}
if not solved then error("the solve failed") end

view.caption("NAFEMS LE10  --  1 MPa ON THE UPPER FACE")
view.time_range(0, 1)

view.on_frame(function(t)
    -- The load going on and coming off again, so the film loops. A full
    -- cosine rather than a ramp for the same reason fem.movie uses one:
    -- a sequence that stops at full load does not join back up.
    local factor = 0.5 * (1.0 - math.cos(2 * math.pi * t))
    view.result{result = solved.result, field = "von_mises", scale = 400 * factor}
    -- A marker at A, the point the benchmark's target stress is read at.
    view.point({2.0, 0.0, 0.3}, 0.12, view.colors.yellow)
    view.label({2.0, 0.0, 0.3}, "A", view.colors.yellow)
    view.axes(1.0)
end)

view.camera{yaw = 35, pitch = 30}
