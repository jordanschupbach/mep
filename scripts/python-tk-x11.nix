# A Python whose tkinter draws through X11 rather than Aqua: what an
# exec-gui block (or :exec) on macOS needs to show a Tk program inside mep
# with no permission asked (gui_embed_xvfb.cpp; nixpkgs builds Tk for Aqua
# on Darwin). Built and put on PATH as python3-x11 by `just python-x11`.
{ pkgs ? import <nixpkgs> { } }:
let
  inherit (pkgs) lib;
  x11 = if pkgs ? libx11 then pkgs.libx11 else pkgs.xorg.libX11;
  tkX11 = pkgs.tk.overrideAttrs (old: {
    configureFlags = (lib.remove "--enable-aqua" old.configureFlags) ++ [
      "--with-x"
      "--x-includes=${lib.getDev x11}/include"
      "--x-libraries=${lib.getLib x11}/lib"
    ];
    buildInputs = (old.buildInputs or [ ]) ++ [ x11 ];
  });
  python = pkgs.python3.override {
    self = python;
    packageOverrides = self: super: { tkinter = super.tkinter.override { tk = tkX11; }; };
  };
in
python.withPackages (p: [ p.tkinter ])
