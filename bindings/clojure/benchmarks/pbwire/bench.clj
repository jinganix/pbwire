;; Clojure binding benchmark against protobuf-java generated code
;; (protoc --java_out, the same oracle the Java bench uses) on matrix3.proto
;; and the four standard tiers from BENCHMARKS.md.
;;
;; The same payload as every other row: every scalar kind, the nested
;; message, the two proto3 optionals, and the repeated fields scaled
;; x0 / x1 / x100 / x3000 (136 / 174 / 2469 / 74913 bytes). Both sides must
;; produce identical bytes before anything is timed, and both reuse a
;; pre-built message per call (encode-message against toByteArray,
;; decode-message plus close against parseFrom), so the comparison is like
;; for like with the Java row. The persistent-map fns (encode/decode) stay
;; for convenience and are gated for parity, but the timed path is the
;; mutable Message (fill once, encode many; decode plus close, no map
;; materialization), which is what beats the generated baseline.
;;
;;   ./bench.sh
(ns pbwire.bench
  (:require [pbwire.core :as pb])
  (:import (java.util Arrays)))

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

(defn- measure [floor op]
  (let [count (calibrate op floor)]
    (run-ns count op)
    (apply min (repeatedly passes #(run-ns count op)))))

(defn- row [impl op nbytes ns]
  (println (format "  %-12s %-7s %12.1f %10.1f" impl op ns (/ (* nbytes 1000.0) ns))))

(defn -main [desc]
  (let [schema (pb/load-schema desc)
        ty (pb/message-type schema "structtest.Scalars3")
        failures (atom 0)]
    (try
      (doseq [tier (range (count cases))]
        (let [label (nth cases tier)
              n (nth repeats tier)
              floor (nth floors tier)
              official (official-message n)
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
                                                      ^bytes (pb/encode ty back))))]
                    (when-not back-ok
                      (println "FAIL" label "pbwire-clojure decode differs")
                      (swap! failures inc)))
                  (let [data-msg (pb/new-message ty)
                        _ (pb/fill-message! data-msg data)
                        tier-count (calibrate #(pb/encode-message data-msg) floor)
                        encode (measure floor #(pb/encode-message data-msg))
                        decode (measure floor #(let [m (pb/decode-message ty reference)]
                                                 (pb/close! m)))
                        official-encode (measure floor #(.toByteArray official))
                        official-decode (measure floor
                                                 #(structtest.Matrix3$Scalars3/parseFrom
                                                   ^bytes reference))]
                    (pb/close! data-msg)
                    (println (str label ": " (count reference) " bytes, "
                                  tier-count " iterations (repeat field x" n ")"))
                    (println (format "  %-12s %-7s %12s %10s"
                                     "impl" "op" "ns/op" "MB/s"))
                    (row "pbwire-clj" "encode" (count reference) encode)
                    (row "protobuf-java" "encode" (count reference) official-encode)
                    (row "pbwire-clj" "decode" (count reference) decode)
                    (row "protobuf-java" "decode" (count reference) official-decode)
                    (println))))))))
      (finally (pb/close! schema)))
    (when (pos? @failures)
      (println @failures "check(s) failed")
      (System/exit 1))))
