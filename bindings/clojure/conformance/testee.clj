;; Official conformance-suite testee through the Clojure binding, mirroring
;; bindings/java/conformance/Testee.java on the same dynamic schema: no
;; per-message code, the request names the type and the descriptor set
;; answers. Binary in -> binary out; JSON/text answer `skipped` (the binding
;; has no printers, like the Python and Ruby testees).
;;
;; Run from bindings/clojure (the parent wires `make conformance-clojure`):
;;   java -cp <clojure jars>:../../java/out/classes:../runtime/src:. \
;;     clojure.main -m testee build/conformance.desc < request > response
(ns testee
  (:require [pbwire.core :as pb])
  (:import (java.io InputStream OutputStream)))

(def ^:private PROTOBUF 1)

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
            type-name (:message_type request)
            payload (:protobuf_payload request)]
        (cond
          (not= PROTOBUF wanted)
          (pb/encode response-ty {:skipped (str "output format " wanted
                                                " is not supported")})

          (nil? payload)
          (pb/encode response-ty {:skipped "only binary input is supported"})

          :else
          (let [t (or (get @types type-name)
                      (let [nt (try (pb/message-type schema type-name)
                                    (catch RuntimeException _ nil))]
                        (when nt (swap! types assoc type-name nt))
                        nt))]
            (if (nil? t)
              (pb/encode response-ty {:runtime_error
                                      (str "unknown message type '"
                                           type-name "'")})
              (let [parsed (try [:ok (pb/decode t payload)]
                                (catch RuntimeException e
                                  [:parse-error (str e)]))]
                (if (= :parse-error (first parsed))
                  (pb/encode response-ty {:parse_error (second parsed)})
                  (try (pb/encode response-ty {:protobuf_payload
                                               (pb/encode t (second parsed))})
                       (catch RuntimeException e
                         (pb/encode response-ty {:serialize_error
                                                 (str e)}))))))))))))

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
