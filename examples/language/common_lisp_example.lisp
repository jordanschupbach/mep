;;;; A small Common Lisp tour: packages, structures, CLOS, conditions and
;;;; restarts, macros, LOOP and FORMAT.
;;;;
;;;;     sbcl --script examples/language/common_lisp_example.lisp
;;;;
;;;; In mep, <leader>uu opens the Common Lisp UI mode (an SBCL console, with
;;;; Symbols/Inspect/Packages/History/Help beside it), <leader>rr loads this
;;;; file into that console, and gh over a symbol shows its documentation.

(defpackage :shapes
  (:use :common-lisp)
  (:export #:shape #:area #:perimeter #:describe-shape))

(in-package :shapes)

;;; Parameters

(defparameter *precision* 2
  "Digits after the decimal point when a measurement is printed.")

(defconstant +tau+ (* 2 pi))

;;; Shapes

(defclass shape ()
  ((name :initarg :name :reader shape-name))
  (:documentation "Anything with an area and a perimeter."))

(defclass circle (shape)
  ((radius :initarg :radius :reader radius)))

(defclass rectangle (shape)
  ((width :initarg :width :reader width)
   (height :initarg :height :reader height)))

(defgeneric area (shape)
  (:documentation "The area SHAPE covers."))

(defgeneric perimeter (shape))

(defmethod area ((c circle)) (* pi (expt (radius c) 2)))
(defmethod area ((r rectangle)) (* (width r) (height r)))
(defmethod perimeter ((c circle)) (* +tau+ (radius c)))
(defmethod perimeter ((r rectangle)) (* 2 (+ (width r) (height r))))

(defmethod print-object ((s shape) stream)
  (print-unreadable-object (s stream :type t)
    (format stream "~a" (shape-name s))))

(defun describe-shape (s &key (stream *standard-output*))
  "Print S's area and perimeter to STREAM."
  (format stream "~&~10a area ~,vf  perimeter ~,vf~%"
          (shape-name s) *precision* (area s) *precision* (perimeter s)))

;;; Conditions and restarts

(define-condition negative-size (error)
  ((value :initarg :value :reader negative-size-value))
  (:report (lambda (c stream)
             (format stream "A size cannot be negative: ~a" (negative-size-value c)))))

(defun checked-size (value)
  "VALUE, unless it is negative -- then offer to use its absolute value."
  (restart-case (if (minusp value) (error 'negative-size :value value) value)
    (use-absolute-value ()
      :report "Use the absolute value instead."
      (abs value))))

(defun make-circle (name radius)
  (make-instance 'circle :name name :radius (checked-size radius)))

;;; A macro

(defmacro with-timing ((label) &body body)
  "Run BODY, then print how long it took under LABEL."
  (let ((start (gensym "START")))
    `(let ((,start (get-internal-real-time)))
       (multiple-value-prog1 (progn ,@body)
         (format t "~&~a took ~d ms~%" ,label
                 (round (* 1000 (- (get-internal-real-time) ,start))
                        internal-time-units-per-second))))))

;;; Putting it together

(defun tally (shapes)
  "A hash table from each shape's class name to how many there are."
  (let ((counts (make-hash-table)))
    (dolist (s shapes counts)
      (incf (gethash (class-name (class-of s)) counts 0)))))

(defun main ()
  (let ((shapes (handler-bind ((negative-size
                                (lambda (c)
                                  (declare (ignore c))
                                  (invoke-restart 'use-absolute-value))))
                  (list (make-circle "unit" 1)
                        (make-circle "flipped" -2)
                        (make-instance 'rectangle :name "door" :width 0.9 :height 2.1)))))
    (with-timing ("describing")
      (mapc #'describe-shape shapes))
    (multiple-value-bind (biggest index)
        (loop with best = nil and where = 0
              for s in shapes
              for i from 0
              for a = (area s)
              when (or (null best) (> a best))
              do (setf best a where i)
              finally (return (values (nth where shapes) where)))
      (format t "Largest: ~a (#~d)~%" biggest index))
    (maphash (lambda (class n) (format t "~(~a~): ~d~%" class n)) (tally shapes))
    (format t "Total area: ~,2f~%" (reduce #'+ shapes :key #'area))))

(main)
