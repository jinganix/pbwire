;; The Clojure binding: plain interop over the Java binding's runtime
;; (bindings/java/runtime, a pure-Java codec with no JNI and no native
;; library). Clojure adds only idiomatic persistent maps in and out plus thin
;; encode/decode fns -- exactly how Kotlin reuses the same runtime. There is
;; no second codec and no second schema path: a FileDescriptorSet is loaded
;; at run time and the Java codec owns the bytes.
;;
;; Maps use keywords or strings for field names (both work, they are looked
;; up by (name k)). Singular integers are longs (uint64 above Long/MAX_VALUE
;; reads as the same raw bits, i.e. negative), floats and doubles are
;; doubles, bytes fields are byte arrays, nested messages are maps, and
;; repeated fields are sequentials. Absent fields are absent from decoded
;; maps; encoding skips nil values.
(ns pbwire.core)

(import 'Schema 'Message 'MessageType)

;; Field enumeration: MessageType keeps its definition in the
;; package-private `def` slot (see MessageType.java), so one reflective read
;; per type exposes the public GenericCodec$MessageDef field table. Cached
;; per type; a schema is read-only after load, so the cache never goes stale.
(defonce ^:private def-slot
  (doto (.getDeclaredField MessageType "def") (.setAccessible true)))

(defonce ^:private field-cache (atom {}))

(defn- type-info
  "Cached {:defs :by-name} for a MessageType: the field table in schema
   order plus a name -> field map, so filling a message never rebuilds it."
  [^MessageType t]
  (let [k (.name t)]
    (or (get @field-cache k)
        (let [defs (mapv (fn [f] {:name (.-fieldName f)
                                  :repeated (.-repeated f)})
                         (.-fields (.get def-slot t)))
              info {:defs defs
                      :by-name (into {} (map (juxt :name identity) defs))}]
          (swap! field-cache assoc k info)
          info))))

(defn load-schema
  "Loads a descriptor set written by `protoc --descriptor_set_out`."
  [path]
  (Schema/fromFile path))

(defn load-schema-bytes
  "Loads descriptor-set bytes; use `--include_imports` so every type resolves."
  [^bytes blob]
  (Schema/fromBytes blob))

(defn message-type
  "Looks up a message by fully qualified name, e.g. \"structtest.Scalars3\"."
  [schema full-name]
  (.message schema full-name))

(defn close!
  "Closes a schema or a message root; child views need no closing."
  [x]
  (.close ^java.lang.AutoCloseable x))

(declare ^:private fill!)

(defn- set-scalar!
  [^Message msg ^String nm v]
  (cond
    (integer? v) (.set msg nm (long v))
    (instance? Boolean v) (.set msg nm (boolean v))
    (number? v) (.set msg nm (double v))
    (string? v) (.set msg nm ^String v)
    (bytes? v) (.set msg nm ^bytes v)
    :else (throw (ex-info (str "cannot encode value for field " nm)
                          {:field nm :value v}))))

(defn- set-one!
  "Sets one field value: a map becomes a nested message, anything else a scalar."
  [^Message msg ^String nm v]
  (if (map? v)
    (fill! (.newMessage msg nm) v)
    (set-scalar! msg nm v)))

(defn- fill!
  "Fills a message from a map in schema order, so encoding is canonical."
  [^Message msg m]
  (let [t (.type msg)
        {:keys [by-name]} (type-info t)]
    (doseq [[k v] m]
      (when (some? v)
        (let [nm (name k)
              f (or (get by-name nm)
                    (throw (ex-info (str "no such field: " nm
                                         " in " (.name t))
                                    {:field nm})))]
          (if (and (sequential? v) (not (:repeated f)))
            (throw (ex-info (str "field " nm " is not repeated")
                            {:field nm}))
            (if (sequential? v)
              (doseq [item v] (set-one! msg nm item))
              (set-one! msg nm v))))))
    msg))

(declare ^:private ->clj)

(defn- ->clj
  "A message view as a persistent map: only present fields, schema order.
   A singular field reads null exactly when absent, so one `get` is both
   the presence test and the read."
  [^Message msg]
  (let [t (.type msg)]
    (into {}
          (keep (fn [{nm :name :keys [repeated]}]
                  (if repeated
                    (let [items (.getList msg ^String nm)]
                      (when (seq items)
                        [(keyword nm) (mapv #(if (instance? Message %)
                                               (->clj ^Message %)
                                               %)
                                            items)]))
                    (let [v (.get msg ^String nm)]
                      (when (some? v)
                        [(keyword nm) (if (instance? Message v)
                                        (->clj ^Message v)
                                        v)]))))
                (:defs (type-info t))))))

(defn encode
  "Encodes a map to wire bytes. Unknown map keys are an error, not silence."
  [^MessageType t m]
  (let [msg (.newMessage t)]
    (try
      (fill! msg m)
      (.encode msg)
      (finally (close! msg)))))

(defn decode
  "Decodes wire bytes to a persistent map. Unknown fields are preserved
   for re-encoding by the codec, but they have no schema names so they do
   not appear in the map; a truncated or malformed payload throws."
  [^MessageType t ^bytes wire]
  (let [msg (.decode t wire)]
    (try
      (->clj msg)
      (finally (close! msg)))))
