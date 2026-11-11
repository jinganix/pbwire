;; Official conformance-suite testee through the Clojure binding, mirroring
;; bindings/java/conformance/Testee.java on the same dynamic schema: no
;; per-message code, the request names the type and the descriptor set
;; answers. Binary, JSON and text in -> binary, JSON or text out; JSPB
;; answers `skipped`, like every other testee here.
;;
;; Run from bindings/clojure (the parent wires `make conformance-clojure`):
;;   java -cp <clojure jars>:../../java/out/classes:../runtime/src:. \
;;     clojure.main -m testee build/conformance.desc < request > response
(ns testee
  (:require [pbwire.core :as pb])
  (:import (java.io InputStream OutputStream)))

(def ^:private PROTOBUF 1)
(def ^:private JSON-IN 2)
(def ^:private TEXT 4)
(def ^:private CATEGORY-IGNORE-UNKNOWN 3)

(defn- read-request [^InputStream in]
  (let [header (byte-array 4)]
    (when (= 4 (.readNBytes in header 0 4))
      (let [size (bit-or (bit-and (aget header 0) 0xFF)
                         (bit-shift-left (bit-and (aget header 1) 0xFF) 8)
                         (bit-shift-left (bit-and (aget header 2) 0xFF) 16)
                         (bit-shift-left (bit-and (aget header 3) 0xFF) 24))
            body (.readNBytes in size)]
        (when (= size (count body)) body)))))

(defn- write-response [^OutputStream out ^bytes payload]
  (let [len (count payload)]
    (.write out (byte-array [(unchecked-byte (bit-and len 0xFF))
                             (unchecked-byte (bit-and (bit-shift-right len 8) 0xFF))
                             (unchecked-byte (bit-and (bit-shift-right len 16) 0xFF))
                             (unchecked-byte (bit-and (bit-shift-right len 24) 0xFF))]))
    (.write out payload)
    (.flush out)))

(defn- handle [types schema response-ty body]
  (let [request (try (pb/decode (pb/message-type
                                 schema "conformance.ConformanceRequest")
                                body)
                     (catch RuntimeException e
                       {:decode-failed (str e)}))]
    (if (contains? request :decode-failed)
      (pb/encode response-ty {:runtime_error
                              (str "cannot parse ConformanceRequest: "
                                   (:decode-failed request))})
      (let [wanted (:requested_output_format request)
            category (:test_category request)
            type-name (:message_type request)
            payload (:protobuf_payload request)
            json (:json_payload request)
            text (:text_payload request)
            print-unknown (:print_unknown_fields request)]
        (if-not (contains? #{PROTOBUF JSON-IN TEXT} wanted)
          (pb/encode response-ty {:skipped (str "output format " wanted
                                                " is not supported")})
          (let [t (or (get @types type-name)
                      (let [nt (try (pb/message-type schema type-name)
                                    (catch RuntimeException _ nil))]
                        (when nt (swap! types assoc type-name nt))
                        nt))]
            (if (nil? t)
              (pb/encode response-ty {:runtime_error
                                      (str "unknown message type '"
                                           type-name "'")})
              ;; A protobuf input decodes to a mutable Message, not a map:
              ;; that path keeps unknown fields and oneof order, so the
              ;; canonical re-encode (and the recommended conformance cases
              ;; that compare against the reference's own output) stay exact.
              ;; JSON/text input still comes in as a map (the binding's
              ;; idiomatic form), which is enough for the JSON/text answers.
              (let [parsed (try
                             (cond
                               (some? json)
                               [:ok nil (pb/from-json t json
                                          (= CATEGORY-IGNORE-UNKNOWN category) nil)]
                               (some? text) [:ok nil (pb/from-text t text nil)]
                               (some? payload) [:ok (pb/decode-message t payload) nil]
                               :else [:skip "unsupported payload"])
                             (catch RuntimeException e
                               [:parse-error (str e)]))]
                (cond
                  (= :skip (first parsed))
                  (pb/encode response-ty {:skipped (second parsed)})

                  (= :parse-error (first parsed))
                  (pb/encode response-ty {:parse_error (second parsed)})

                  :else
                  (let [[_ msg m] parsed]
                    (try
                      (pb/encode response-ty
                        (cond
                          (= TEXT wanted)
                          {:text_payload (if msg
                                           (.toText msg (boolean print-unknown))
                                           (pb/text-of-map t m (boolean print-unknown)))}
                          (= JSON-IN wanted)
                          {:json_payload (if msg
                                           (.toJson msg)
                                           (pb/json-of-map t m))}
                          :else
                          {:protobuf_payload (if msg
                                               (pb/encode-message msg)
                                               (pb/encode t m))}))
                      (catch RuntimeException e
                        (pb/encode response-ty {:serialize_error (str e)}))
                      (finally (when msg (pb/close! msg))))))))))))))

(defn -main [desc]
  (let [schema (pb/load-schema desc)
        response-ty (pb/message-type schema "conformance.ConformanceResponse")
        types (atom {})
        in System/in
        out System/out]
    (loop []
      (when-let [body (read-request in)]
        (let [payload (try (handle types schema response-ty body)
                           (catch RuntimeException e
                             (pb/encode response-ty {:runtime_error
                                                     (str e)})))]
          (write-response out payload)
          (recur))))))
