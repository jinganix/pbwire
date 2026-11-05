;; Runnable round trip: build structtest.Scalars3 as a plain map, encode,
;; compare with the pinned protoc bytes (bindings/c/tests/data/matrix3.bin),
;; decode and print. The schema is the checked-in FileDescriptorSet, so no
;; protoc run is needed.
;;
;;   java -cp <jars>:../../java/out/classes:../runtime/src:. \
;;     clojure.main -m roundtrip [matrix3.desc]
(ns roundtrip
  (:require [clojure.java.io :as io]
            [pbwire.core :as pb])
  (:import (java.nio.file Files)
           (java.util Arrays)))

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
  (let [desc (or (first args) "../c/tests/data/matrix3.desc")
        dir (.getParent (io/file desc))
        reference (Files/readAllBytes (.toPath (io/file dir "matrix3.bin")))
        schema (pb/load-schema desc)
        ty (pb/message-type schema "structtest.Scalars3")]
    (try
      (let [wire (pb/encode ty payload)]
        (if (Arrays/equals ^bytes reference ^bytes wire)
          (println "encode matches protoc," (count wire) "bytes")
          (do (println "MISMATCH: encode differs from" (str dir "/matrix3.bin"))
              (System/exit 1)))
        (let [back (pb/decode ty wire)]
          (println "round trip: str =" (pr-str (:str back))
                   ", inner =" (pr-str (:inner back))
                   ", inners =" (count (:inners back)))))
      (finally (pb/close! schema)))))
