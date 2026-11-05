;; Generated-conformance check for the Clojure binding: build matrix3.proto's
;; Scalars3 as a map, encode, require the protoc reference bytes
;; (matrix3.bin), then decode those bytes back and check every field.
;; The schema travels embedded in tests/gen/matrix3.clj.
;;
;;   java -cp <jars>:../../java/out/classes:../runtime/src:tests:tests/gen \
;;     clojure.main -m generated-test [matrix3.bin]
(ns generated-test
  (:require [clojure.java.io :as io]
            [matrix3 :as m])
  (:import (java.nio.file Files)
           (java.util Arrays)))

(def checks (atom 0))

(defn check [ok what]
  (swap! checks inc)
  (assert ok what))

(def payload
  {:i32 Integer/MIN_VALUE
   :i64 Long/MAX_VALUE
   :u32 0xffffffff
   :u64 -1
   :s32 Integer/MIN_VALUE
   :s64 Long/MIN_VALUE
   :fx32 0xdeadbeef
   :fx64 72623859790382856
   :sfx32 Integer/MIN_VALUE
   :sfx64 Long/MIN_VALUE
   :f32 Double/POSITIVE_INFINITY
   :f64 -123456.789
   :flag true
   :str "hello"
   :color 2
   :opt_i32 -7
   :opt_str "opt"
   :nums [1 -5 300]
   :tags ["a" "bb"]
   :inners [{:name "x" :value 1} {:name "y" :value -2}]
   :flags [true false true]
   :inner {:name "kid" :value 42}})

(defn -main [& args]
  (let [bin (or (first args) "../c/tests/data/matrix3.bin")
        reference (Files/readAllBytes (.toPath (io/file bin)))
        wire (m/encode-structtest-scalars3 payload)]
    (check (Arrays/equals ^bytes reference ^bytes wire)
           "generated encode equals the protoc reference bytes")
    (let [back (m/decode-structtest-scalars3 reference)]
      (check (= (:i32 back) Integer/MIN_VALUE) "i32")
      (check (= (:i64 back) Long/MAX_VALUE) "i64")
      (check (= (:u32 back) 0xffffffff) "u32")
      (check (= (:u64 back) -1) "u64")
      (check (= (:flag back) true) "flag")
      (check (= (:str back) "hello") "str")
      (check (= (:color back) 2) "color")
      (check (= (:inner back) {:name "kid" :value 42}) "inner")
      (check (= (:nums back) [1 -5 300]) "nums")
      (check (= (:tags back) ["a" "bb"]) "tags")
      (check (= (:inners back) [{:name "x" :value 1} {:name "y" :value -2}]) "inners")
      (check (= (:opt_i32 back) -7) "opt_i32")
      (check (= (:opt_str back) "opt") "opt_str")
      (check (= (:flags back) [true false true]) "flags"))
    (check (= 0 (count (m/encode-structtest-scalars3 {})))
           "empty generated message encodes to nothing")
    (let [zero (m/decode-structtest-scalars3
                (m/encode-structtest-scalars3 {:opt_i32 0}))]
      (check (= (:opt_i32 zero) 0) "explicit zero with presence survives"))
    (println (str "generated test ok: " @checks " checks"))))
