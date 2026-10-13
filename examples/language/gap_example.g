# A small GAP tour: functions, lists, records, and a little group theory.
# Run with `gap -q`; QUIT at the end so the script exits instead of
# dropping into GAP's prompt.

Distance := function(a, b)
    return Sqrt(Float((a.x - b.x)^2 + (a.y - b.y)^2));
end;

Print("hello, world\n");

a := rec(x := 0, y := 0);
b := rec(x := 3, y := 4);
Print("distance = ", Distance(a, b), "\n");

nums := [1 .. 5];
Print("sum = ", Sum(nums), "\n");
Print("squares = ", List(nums, n -> n^2), "\n");
Print("evens = ", Filtered(nums, IsEvenInt), "\n");

# Group theory: the symmetric group on 4 points.
G := SymmetricGroup(4);
Print("|S4| = ", Size(G), "\n");
Print("S4 is solvable: ", IsSolvable(G), "\n");
Print("conjugacy classes: ", Length(ConjugacyClasses(G)), "\n");
Print("(1,2,3)*(3,4) = ", (1,2,3) * (3,4), "\n");

QUIT;
