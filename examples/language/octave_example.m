% A small Octave/MATLAB script: vectors, anonymous functions, a struct and
% a cellfun. Runs headless (octave-cli), so nothing here opens a figure.

1;  % marks this file as a script, so the functions below can live in it

function d = distance(a, b)
  d = sqrt((a.x - b.x)^2 + (a.y - b.y)^2);
end

disp('hello, world')

a = struct('x', 0, 'y', 0);
b = struct('x', 3, 'y', 4);
printf('distance = %.2f\n', distance(a, b));

nums = 1:5;
printf('sum = %d\n', sum(nums));

square = @(v) v .^ 2;
disp(['squares = ', mat2str(square(nums))]);

A = magic(3);
printf('det(magic(3)) = %g\n', det(A));

words = {'alpha', 'beta', 'gamma'};
lengths = cellfun(@numel, words);
for i = 1:numel(words)
  printf('%s has %d letters\n', words{i}, lengths(i));
end
