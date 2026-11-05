;; Self-check against the pinned protoc bytes: build structtest.Scalars3
;; from bindings/c/tests/data/matrix3.txt, encode, compare with matrix3.bin
;; (the same oracle the C binding uses), then decode it back and check the
;; values. Plus the edge payload (tests/data/edge.bin) the host bindings
;; share: NUL bytes, non-ASCII text, a negative enum, empty elements, oneof.
(ns pbwire.test-pbwire
  (:require [clojure.test :refer [deftest is run-tests]]
            [clojure.java.io :as io]
            [pbwire.core :as pb])
  (:import (java.nio.file Files)
           (java.util Arrays)))

(def matrix3-fill
  {:i32 Integer/MIN_VALUE
   :i64 Long/MAX_VALUE
   :u32 0xffffffff
   :u64 -1 ;; u64::MAX bits
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

(defn- slurp-bytes [path]
  (Files/readAllBytes (.toPath (io/file path))))

(defn- bytes= [^bytes a ^bytes b]
  (Arrays/equals a b))

(deftest matrix3-round-trip
  (let [desc (or (System/getenv "PBWIRE_MATRIX3_DESC")
                 "../c/tests/data/matrix3.desc")
        dir (.getParent (io/file desc))
        oracle (slurp-bytes (str dir "/matrix3.bin"))
        schema (pb/load-schema desc)
        ty (pb/message-type schema "structtest.Scalars3")
        inner-ty (pb/message-type schema "structtest.Inner3")]
    (try
      (is (bytes= oracle (pb/encode ty matrix3-fill))
          "encoded bytes differ from protoc")
      (let [back (pb/decode ty oracle)]
        (is (= Integer/MIN_VALUE (:i32 back)))
        (is (= true (:flag back)))
        (is (= "hello" (:str back)))
        (is (= -123456.789 (:f64 back)))
        (is (= 2 (:color back)))
        (is (contains? back :opt_str))
        (is (= [1 -5 300] (:nums back)))
        (is (= ["a" "bb"] (:tags back)))
        (is (= [true false true] (:flags back)))
        (is (= "kid" (get-in back [:inner :name])))
        (is (= 2 (count (:inners back))))
        (is (bytes= oracle (pb/encode ty back))
            "decode -> encode is not byte-exact"))
      ;; A field the schema does not declare (99, varint 7) has no name,
      ;; so it decodes to nothing -- and an unknown map key is refused.
      (let [unknown (byte-array [(unchecked-byte 0x98) 0x06 0x07])]
        (is (= {} (pb/decode inner-ty unknown)))
        (is (thrown? Exception (pb/encode inner-ty {:nope 1}))))
      ;; Bad input is an error, not a crash.
      (is (thrown? Exception (pb/decode ty (byte-array [(unchecked-byte 0x80)])))
          "truncated tag accepted")
      (is (thrown? Exception (pb/message-type schema "structtest.Nope"))
          "unknown message accepted")
      (finally (pb/close! schema)))))

(deftest edge-payload
  (let [root "../.."
        oracle (slurp-bytes (str root "/tests/data/edge.bin"))
        schema (pb/load-schema (str root "/tests/data/edge.desc"))
        ty (pb/message-type schema "edge.Edge")]
    (try
      (let [back (pb/decode ty oracle)]
        (is (bytes= (byte-array [0 1 (unchecked-byte 0xff)]) (:raw back)))
        (is (= "héllo ☃" (:text back)))
        (is (= -3 (:sign back)))
        (is (= ["a" "" "b"] (:tags back)))
        (is (= -1 (get-in back [:inner :n])))
        (is (contains? back :maybe))
        (is (contains? back :pick_str))
        (is (not (contains? back :pick_num)) "oneof keeps only the last member")
        (is (bytes= oracle (pb/encode ty back))
            "edge decode -> encode is not byte-exact"))
      ;; A default message is zero bytes both ways.
      (is (zero? (count (pb/encode ty {}))))
      (let [empty (pb/decode ty (byte-array 0))]
        (is (= {} empty))
        (is (not (contains? empty :text))))
      ;; A wrong value kind is refused, not written.
      (is (thrown? Exception (pb/encode ty {:pick_num "not a number"})))
      (is (thrown? Exception (pb/encode ty {:text {:nested "map"}})))
      (finally (pb/close! schema)))))

(defn -main [& args]
  (let [{:keys [fail error]} (run-tests 'pbwire.test-pbwire)]
    (System/exit (if (zero? (+ fail error)) 0 1))))
