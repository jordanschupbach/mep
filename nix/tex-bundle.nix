# mep-tex's TeX bundle (src/tex/tex_bundle.h): the TeX Live packages
# mep's own engine carries, so builtin compiles need no TeX installation.
# The tree is put together by nixpkgs' TeX Live and packed by
# tools/tex_bundle.py into $out/tex-bundle.mtb, with a manifest of every
# package in it and its licence (MANIFEST.json, LICENSES.txt inside).
#
# What is in it: what mep's own LaTeX output asks for (the org and mepml
# exports, maths previews through standalone, Beamer decks), the common
# packages a hand-written article reaches for, Latin Modern, the CM/AMS
# fonts and psnfss's PostScript fonts. Anything else comes from a system
# TeX Live with tex_packages = bundle+system.
{ pkgs, bundleScript }:

let
  tl = pkgs.texlive;
  names = [
    "scheme-basic"     # LaTeX kernel, amsmath, graphics, hyperref, geometry, tools, babel, CM/AMS fonts
    "lm" "ec"          # Latin Modern; EC metrics (drawn by Latin Modern: see tools/tex_bundle.py)
    "xcolor" "ulem" "fancyvrb" "float" "upquote"
    "tcolorbox" "pgf" "tikzfill" "pdfcol" "environ" "trimspaces"
    "standalone" "xkeyval" "preview" "varwidth"
    "beamer"
    "booktabs" "caption" "enumitem" "mathtools" "listings" "multirow" "cleveref"
    "wrapfig" "setspace" "parskip" "microtype" "csquotes" "siunitx" "lastpage" "titlesec"
    # psnfss's fonts (times, helvet, mathptmx, mathpazo ...): URW's
    # PostScript 35, Charter and Utopia, through virtual fonts
    "times" "helvetic" "courier" "palatino" "avantgar" "bookman" "ncntrsbk" "zapfchan"
    "mathpazo" "fpl" "charter" "utopia" "rsfs"
  ];
  tree = tl.combine (pkgs.lib.genAttrs names (n: tl.${n}));

  # Every package in the tree (the closure of `names` through their
  # TeX Live dependencies), for the manifest.
  runPkg = n: builtins.head tl.${n}.pkgs;
  closure = builtins.genericClosure {
    startSet = map (n: { key = n; drv = runPkg n; }) names;
    operator = item: map (d: { key = d.pname; drv = d; }) (item.drv.tlDeps or [ ]);
  };
  licence = l: l.spdxId or l.shortName or "unknown";
  manifest = map (item: {
    name = item.key;
    version = item.drv.version or "";
    licenses = map licence (pkgs.lib.toList (item.drv.meta.license or [ ]));
  }) closure;
  manifestFile = pkgs.writeText "tex-bundle-manifest.json" (builtins.toJSON manifest);
in
pkgs.runCommand "mep-tex-bundle" {
  nativeBuildInputs = [ pkgs.python3 ];
  # The TeX Live it was made from, and the same with cm-super (the fonts
  # the bundle leaves out): references to compare mep-tex with
  # (tools/tex_compare.py).
  passthru.tree = tree;
  passthru.reference = tl.combine (pkgs.lib.genAttrs (names ++ [ "cm-super" ]) (n: tl.${n}));
} ''
  mkdir -p $out
  python3 ${bundleScript} \
    --tree ${tree}/share/texmf-var \
    --tree ${tree}/share/texmf \
    --manifest ${manifestFile} \
    --source "TeX Live (nixpkgs ${pkgs.lib.version})" \
    --out $out/tex-bundle.mtb
''
