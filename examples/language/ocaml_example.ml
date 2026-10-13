(* A small OCaml tour: a record, a variant with pattern matching, and List
   functions. Run as a script by the `ocaml` toplevel, no build needed. *)

type point = { x : float; y : float }

let distance a b = Float.hypot (a.x -. b.x) (a.y -. b.y)

type shape = Circle of float | Rect of float * float

let area = function
  | Circle r -> Float.pi *. r *. r
  | Rect (w, h) -> w *. h

let name = function Circle _ -> "circle" | Rect _ -> "rect"

let () =
  print_endline "hello, world";
  let a = { x = 0.0; y = 0.0 } and b = { x = 3.0; y = 4.0 } in
  Printf.printf "distance = %.2f\n" (distance a b);
  let nums = [ 1; 2; 3; 4; 5 ] in
  Printf.printf "sum = %d\n" (List.fold_left ( + ) 0 nums);
  let squares = List.map (fun n -> n * n) nums in
  Printf.printf "squares = %s\n" (String.concat ", " (List.map string_of_int squares));
  List.iter
    (fun s -> Printf.printf "%s area = %.2f\n" (name s) (area s))
    [ Circle 1.0; Rect (2.0, 3.0) ]
