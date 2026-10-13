# Evaluated, not built: <leader>rr runs `nix eval --file` on this file and
# prints the resulting value.
let
  point = x: y: { inherit x y; };

  distance = a: b:
    let
      dx = a.x - b.x;
      dy = a.y - b.y;
      # Nix has no sqrt builtin -- a few Newton steps are plenty here.
      sqrt = n: builtins.foldl' (g: _: (g + n / g) / 2.0) n (builtins.genList (i: i) 20);
    in
    sqrt (1.0 * (dx * dx + dy * dy));

  nums = [ 1 2 3 4 5 ];
in
{
  greeting = "hello, world";
  distance = distance (point 0 0) (point 3 4);
  sum = builtins.foldl' builtins.add 0 nums;
  squares = map (n: n * n) nums;
  evens = builtins.filter (n: builtins.bitAnd n 1 == 0) nums;
  shout = builtins.replaceStrings [ "hello" ] [ "HELLO" ] "hello, nix";
}
