;; A small Scheme tour (Guile): association lists, named let, higher-order
;; procedures, and a tail-recursive loop.

(use-modules (srfi srfi-1) (ice-9 format))

(define (make-point x y) (cons x y))
(define (point-x p) (car p))
(define (point-y p) (cdr p))

(define (distance a b)
  (let ((dx (- (point-x a) (point-x b)))
        (dy (- (point-y a) (point-y b))))
    (sqrt (+ (* dx dx) (* dy dy)))))

(define (sum values) (fold + 0 values))

(display "hello, world")
(newline)

(format #t "distance = ~,2f~%" (distance (make-point 0 0) (make-point 3 4)))

(define nums (iota 5 1))
(format #t "sum = ~a~%" (sum nums))
(format #t "squares = ~a~%" (map (lambda (n) (* n n)) nums))
(format #t "evens = ~a~%" (filter even? nums))

(let loop ((i 1) (acc 1))
  (if (> i 5)
      (format #t "5! = ~a~%" acc)
      (loop (+ i 1) (* acc i))))
