;; Clojure binding benchmark against protobuf-java generated code
;; (protoc --java_out, the same oracle the Java bench uses) on matrix3.proto
;; and the four standard tiers from BENCHMARKS.md.
;;
;; The same payload as every other row: every scalar kind, the nested
;; message, the two proto3 optionals, and the repeated fields scaled
;; x0 / x1 / x100 / x3000 (136 / 174 / 2469 / 74913 bytes). Every row's
;; bytes are gated against the official runtime before anything is timed.
;;
;; Arms (bindings/README.md#pbench-three-arms-one-contract):
;;   tree  the persistent-map API, fresh map per iteration -- the full task:
;;         a fresh (payload n) per iteration, encode; decode, every
;;         gate-checked field read back per iteration (consume-tree)
;;   core  the mutable-message fast path       -- native object in and out, no map walk
;;   proxy the prebuilt-map task, relabelled   -- held map re-encoded,
;;         decode-and-drop with no read-back. Published, never the headline.
;;   shim  ../java/pbench/jni                  -- ceiling: one JNI crossing, no tree translation
;;   core-gen the committed pbwire Java generated model (Structtest_Scalars3
;;         Builder + toByteArray fresh per iteration; parseFrom + the same
;;         gate-checked deep read as consume-core) -- Task 2 experimental arm.
;;
;; The persistent-map fns (encode/decode) are the `tree` row: the map is
;; the binding's own tree, so its walk and materialization are the arm's
;; cost. The `core` row is the mutable Message (fill once, encode many;
;; decode plus close, no map materialization), which is what beats the
;; generated baseline. The review focus for this plan says the core row is
;; that fast path, not a per-call map conversion.
;;
;;   ./bench.sh
(ns pbwire.bench
  (:require [matrix3 :as gen]
            [pbwire.core :as pb])
  (:import (java.util Arrays) (structtest Matrix3$Scalars3 Matrix3$Inner3) Message
           Structtest_Scalars3 Structtest_Inner3 Structtest_Color3
           Structtest_Scalars3$Builder Structtest_Inner3$Builder))

(def ^:private cases ["small" "medium" "large" "xlarge"])
(def ^:private repeats [0 1 100 3000])
(def ^:private want [136 174 2469 74913])
(def ^:private floors [50000 50000 5000 500])
(def ^:private min-pass-secs 0.25)
(def ^:private passes 3)

(defn- base-payload []
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
   :inner {:name "kid" :value 42}})

(defn- payload [n]
  (assoc (base-payload)
         :nums (mapv #(- % 3) (range n))
         :tags (vec (repeat n "tag"))
         :inners (mapv (fn [i] {:name "grandkid" :value (inc i)}) (range n))
         :flags (mapv even? (range n))))

(defn- official-message [n]
  (let [b (structtest.Matrix3$Scalars3/newBuilder)
        inner (-> (structtest.Matrix3$Inner3/newBuilder)
                  (.setName "kid")
                  (.setValue 42)
                  .build)]
    (.setI32 b Integer/MIN_VALUE)
    (.setI64 b Long/MAX_VALUE)
    (.setU32 b (unchecked-int 0xffffffff))
    (.setU64 b -1)
    (.setS32 b Integer/MIN_VALUE)
    (.setS64 b Long/MIN_VALUE)
    (.setFx32 b (unchecked-int 0xdeadbeef))
    (.setFx64 b 72623859790382856)
    (.setSfx32 b Integer/MIN_VALUE)
    (.setSfx64 b Long/MIN_VALUE)
    (.setF32 b Float/POSITIVE_INFINITY)
    (.setF64 b -123456.789)
    (.setFlag b true)
    (.setStr b "hello")
    (.setColor b structtest.Matrix3$Color3/COLOR3_GREEN)
    (.setInner b inner)
    (.setOptI32 b -7)
    (.setOptStr b "opt")
    (dotimes [i n]
      (.addNums b (unchecked-int (- i 3)))
      (.addTags b "tag")
      (.addInners b (-> (structtest.Matrix3$Inner3/newBuilder)
                        (.setName "grandkid")
                        (.setValue (inc i))
                        .build))
      (.addFlags b (even? i)))
    (.build b)))

(defn- run-ns [n op]
  (let [start (System/nanoTime)]
    (dotimes [_ n] (op))
    (/ (double (- (System/nanoTime) start)) n)))

(defn- calibrate [op floor]
  (loop [count (max 1 floor)]
    (let [start (System/nanoTime)]
      (dotimes [_ count] (op))
      (if (or (>= (/ (double (- (System/nanoTime) start)) 1e9) min-pass-secs)
              (>= count 100000000))
        count
        (recur (* 2 count))))))

(defn- measure-all [floor ops]
  (let [counts (mapv #(calibrate % floor) ops)
        best (atom (vec (repeat (count ops) Double/MAX_VALUE)))]
    (doseq [[c op] (map vector (mapv #(min % 1000) counts) ops)]
      (run-ns c op))
    (dotimes [_ passes]
      (doseq [i (range (count ops))]
        (swap! best #(assoc % i (min (nth % i) (run-ns (nth counts i) (nth ops i)))))))
    @best))

(defn- row [impl op nbytes ns]
  (println (format "  %-18s %-7s %12.1f %10.1f" impl op ns (/ (* nbytes 1000.0) ns))))

(def ^:private sink (atom 0))

(defn- consume-tree
  "The tree decode half of the timed full task: decode to a persistent
   map, then read back every gate-checked field (the same field list the
   gate asserts), so the timer covers the materialization."
  [ty wire n]
  (let [back (pb/decode ty wire)]
    (swap! sink +
           (count (:str back))
           (count (:tags back))
           (count (:inners back))
           (count (get-in back [:inner :name]))
           (get-in back [:inner :value]))
    (when (pos? n)
      (let [last-inner (last (:inners back))]
        (swap! sink + (count (:name last-inner)) (:value last-inner))))))

(defn- consume-core
  "The core decode half: mutable decode plus the same gate-checked
   read-back as consume-tree, so core vs protobuf-java is deep vs deep.
   Every interop target is hinted: an unhinted local compiles to a
   reflective call on the timed path, and (last inners) walks the whole
   repeated field where an indexed get keeps the same read."
  [ty wire n]
  (let [^Message m (pb/decode-message ty wire)]
    (try
      (let [s (.get m "str")
            ^Message inner (.get m "inner")
            ^java.util.List tags (.get m "tags")
            ^java.util.List inners (.get m "inners")
            iname (when inner (.get inner "name"))
            ivalue (when inner (.get inner "value"))]
        (swap! sink + (count s) (count tags) (count inners)
               (count iname) (long (or ivalue 0)))
        (when (pos? n)
          (let [^Message last-inner (.get inners (dec n))]
            (swap! sink + (count (.get last-inner "name"))
                   (long (.get last-inner "value"))))))
      (finally (pb/close! m)))))

(defn- consume-baseline
  "The baseline decode half of the timed full task, mirroring
   bindings/java/benchmarks/Bench.java consumeBaseline (Task 5): the pbwire
   core arm materializes every gate-checked field, so the protobuf-java arm
   folds the same gate-checked fields instead of timing a bare parse
   (fairness 4). Task 5 measured nil effect (protobuf-java strings parse
   eagerly); this keeps the two harnesses' headline cells comparable."
  [^bytes wire n]
  (let [^Matrix3$Scalars3 back (structtest.Matrix3$Scalars3/parseFrom wire)]
    (swap! sink + (.length (.getStr back))
           (.getTagsCount back)
           (.getInnersCount back)
           (.length (.getName (.getInner back)))
           (.getValue (.getInner back)))
    (when (pos? n)
      (let [^Matrix3$Inner3 m (.getInners back (dec n))]
        (swap! sink + (.length (.getName m)) (.getValue m))))))

(defn- core-gen-message
  "Fresh-task build via the committed pbwire Java generated model
   (bindings/java/tests/gen), mirroring official-message field for field.
   Every interop target is hinted: an unhinted Builder local compiles to
   a reflective call on the timed path, and the multi-overload add/set
   forms need hinted args to resolve at compile time."
  [n]
  (let [^Structtest_Scalars3$Builder b (Structtest_Scalars3/newBuilder)
        ^Structtest_Inner3 kid (let [^Structtest_Inner3$Builder ib (Structtest_Inner3/newBuilder)]
                                 (.setName ib "kid")
                                 (.setValue ib 42)
                                 (.build ib))]
    (.setI32 b Integer/MIN_VALUE)
    (.setI64 b Long/MAX_VALUE)
    (.setU32 b (unchecked-int 0xffffffff))
    (.setU64 b -1)
    (.setS32 b Integer/MIN_VALUE)
    (.setS64 b Long/MIN_VALUE)
    (.setFx32 b (unchecked-int 0xdeadbeef))
    (.setFx64 b 72623859790382856)
    (.setSfx32 b Integer/MIN_VALUE)
    (.setSfx64 b Long/MIN_VALUE)
    (.setF32 b Float/POSITIVE_INFINITY)
    (.setF64 b -123456.789)
    (.setFlag b true)
    (.setStr b "hello")
    (.setColor b Structtest_Color3/COLOR3_GREEN)
    (.setInner b kid)
    (.setOptI32 b -7)
    (.setOptStr b "opt")
    (dotimes [i n]
      (.addNums b (unchecked-int (- i 3)))
      (.addTags b "tag")
      (let [^Structtest_Inner3$Builder ib (Structtest_Inner3/newBuilder)
            ^Structtest_Inner3 g (do (.setName ib "grandkid")
                                      (.setValue ib (unchecked-int (inc i)))
                                      (.build ib))]
        (.addInners b g))
      (.addFlags b (even? i)))
    (.build b)))

(defn- core-gen-encode
  "Timed encode op: fresh Builder build plus toByteArray per iteration,
   the same fresh-task shape as the protobuf-java row."
  [n]
  (let [^Structtest_Scalars3 m (core-gen-message n)]
    (.toByteArray m)))

(defn- core-gen-roundtrip-ok?
  "Decode gate: parseFrom plus the same field checks as the map back-ok
   gate, closed by a re-encode byte comparison."
  [^bytes reference n]
  (let [^Structtest_Scalars3 back (Structtest_Scalars3/parseFrom reference)
        ^Structtest_Inner3 inner (.getInner back)]
    (and (= "hello" (.getStr back))
         (= "kid" (.getName inner))
         (= n (.getNumsCount back))
         (or (zero? n)
             (= (- n 4) (.getNums back (dec n))))
         (Arrays/equals ^bytes reference ^bytes (.toByteArray back)))))

(defn- consume-core-gen
  "The core-gen decode half of the timed full task: parseFrom plus the
   same gate-checked deep read as consume-baseline (str, tags/inners
   counts, inner name/value, indexed last-inner read), so core-gen vs
   protobuf-java is deep vs deep. Every interop target is hinted: an
   unhinted local compiles to a reflective call on the timed path, and an
   unhinted intermediate (e.g. (.getName (.getInner back))) reflects even
   when the outer target is hinted, hence the bound inner locals."
  [^bytes wire n]
  (let [^Structtest_Scalars3 back (Structtest_Scalars3/parseFrom wire)
        ^Structtest_Inner3 inner (.getInner back)]
    (swap! sink + (.length (.getStr back))
           (.getTagsCount back)
           (.getInnersCount back)
           (.length (.getName inner))
           (.getValue inner))
    (when (pos? n)
      (let [^Structtest_Inner3 m (.getInners back (dec n))]
        (swap! sink + (.length (.getName m)) (.getValue m))))))

(defn -main [desc]
  (let [schema (pb/load-schema desc)
        ty (pb/message-type schema "structtest.Scalars3")
        failures (atom 0)]
    (try
      (doseq [tier (range (count cases))]
        (let [label (nth cases tier)
              n (nth repeats tier)
              floor (nth floors tier)
              ;; Hinted so the timed baseline below compiles to a direct
              ;; call: an unhinted local reflects (~800 ns/call here) and
              ;; never warms, which once inflated this row to 9.9x.
              ^Matrix3$Scalars3 official (official-message n)
              reference (.toByteArray official)]
          (if (not= (count reference) (nth want tier))
            (do (println "FAIL" label "official encoder wrote" (count reference)
                         "bytes, expected" (nth want tier))
                (swap! failures inc))
            (let [data (payload n)
                  got (pb/encode ty data)]
              (if (not (Arrays/equals ^bytes got ^bytes reference))
                (do (println "FAIL" label "pbwire-clojure wrote" (count got)
                             "bytes, protobuf-java" (count reference))
                    (swap! failures inc))
                (do
                  (println "OK  " (str label " encoding parity, x" n
                                       " (" (count reference) " bytes)"))
                  (let [back-ok (let [back (pb/decode ty reference)]
                                  (and (= "hello" (:str back))
                                       (= "kid" (get-in back [:inner :name]))
                                       (= n (count (:nums back)))
                                       (or (zero? n)
                                           (= (- n 4) (last (:nums back))))
                                       (Arrays/equals ^bytes reference
                                                      ^bytes (pb/encode ty back))))
                        gen-ok (Arrays/equals
                                ^bytes reference
                                ^bytes (gen/encode-structtest-scalars3 data))
                        ;; Full-task tree gate: a fresh per-iteration build must
                        ;; match the reference too (not just the held map above).
                        fresh-ok (Arrays/equals
                                  ^bytes reference
                                  ^bytes (gen/encode-structtest-scalars3
                                            (payload n)))
                        ;; Task 2 core-gen gate: the committed Java generated
                        ;; model must match the reference too (encode) and
                        ;; round-trip it (decode), same bytes both directions.
                        core-gen-ok (Arrays/equals
                                    ^bytes reference
                                    ^bytes (core-gen-encode n))
                        core-gen-back-ok (core-gen-roundtrip-ok? reference n)]
                    (when-not back-ok
                      (println "FAIL" label "pbwire-clojure decode differs")
                      (swap! failures inc))
                    (when-not gen-ok
                      (println "FAIL" label "generated model bytes differ")
                      (swap! failures inc))
                    (when-not fresh-ok
                      (println "FAIL" label "fresh-build tree bytes differ")
                      (swap! failures inc))
                    (when-not core-gen-ok
                      (println "FAIL" label "core-gen model bytes differ")
                      (swap! failures inc))
                    (when-not core-gen-back-ok
                      (println "FAIL" label "core-gen decode differs")
                      (swap! failures inc))
                    (when (and back-ok gen-ok fresh-ok core-gen-ok core-gen-back-ok)
                      (println "OK  " (str label " generated-model parity, x" n
                                           " (" (count reference) " bytes)"))))
                  ;; Tree ops are the Go-parity full task: a fresh map per
                  ;; iteration, deep materialization of every gate-checked field
                  ;; on decode (consume-tree). Proxy ops are the old prebuilt
                  ;; task, relabelled, not re-measured as a new task.
                  ;; Calibration stays per-op (measure-all): the fast proxy ops
                  ;; keep their own count and never inflate the tree rows.
                  (let [tier-count (calibrate #(gen/encode-structtest-scalars3
                                                 (payload n)) floor)
                        res (measure-all floor
                              [#(gen/encode-structtest-scalars3 (payload n))
                               #(let [m (pb/new-message ty)]
                                  (try
                                    (pb/fill-message! m (payload n))
                                    (pb/encode-message m)
                                    (finally (pb/close! m))))
                               #(gen/encode-structtest-scalars3 data)
                               #(let [^Matrix3$Scalars3 fresh__ (official-message n)]
                                  (.toByteArray fresh__))
                               #(consume-tree ty reference n)
                               #(consume-core ty reference n)
                               #(gen/decode-structtest-scalars3 reference)
                               #(consume-baseline reference n)
                               #(core-gen-encode n)
                               #(consume-core-gen reference n)])]
                    (println (str label ": " (count reference) " bytes, "
                                  tier-count " iterations (repeat field x" n ")"))
                    (println (format "  %-18s %-7s %12s %10s"
                                     "impl" "op" "ns/op" "MB/s"))
                    (row "pbwire-clj-tree" "encode" (count reference) (nth res 0))
                    (row "pbwire-clj-core" "encode" (count reference) (nth res 1))
                    (row "pbwire-clj-proxy" "encode" (count reference) (nth res 2))
                    (row "protobuf-java" "encode" (count reference) (nth res 3))
                    (row "pbwire-clj-tree" "decode" (count reference) (nth res 4))
                    (row "pbwire-clj-core" "decode" (count reference) (nth res 5))
                    (row "pbwire-clj-proxy" "decode" (count reference) (nth res 6))
                    (row "protobuf-java" "decode" (count reference) (nth res 7))
                    (row "pbwire-clj-core-gen" "encode" (count reference) (nth res 8))
                    (row "pbwire-clj-core-gen" "decode" (count reference) (nth res 9))
                    (println))))))))
      (finally (pb/close! schema)))
    (when (pos? @failures)
      (println @failures "check(s) failed")
      (System/exit 1))))
