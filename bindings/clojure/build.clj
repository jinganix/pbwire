(ns build
  (:require [clojure.tools.build.api :as b]
            [clojure.string :as str]))

;; Group/artifact decided once: io.github.jinganix, matching the Java/Kotlin
;; jobs on Maven Central. Clojars claims the group on first deploy.
(def lib 'io.github.jinganix/pbwire-clojure)
;; The version comes from the repo-root VERSION file (like the composer gemspec
;; and podspec patterns); sync-versions.sh only asserts build.clj reads it.
(def version (-> "../../VERSION" slurp str/trim))
(def class-dir "target/classes")
(def basis (delay (b/create-basis {:project "deps.edn"})))
(def jar-file (format "target/%s-%s.jar" (name lib) version))

(defn clean [_]
  (b/delete {:path "target"}))

(defn jar [_]
  (clean nil)
  ;; The Clojure binding calls the Java runtime compiled from source, so the
  ;; published jar bundles those classes: no pbwire-java dependency needed.
  (b/javac {:src-dirs ["../java/runtime"]
            :class-dir class-dir
            :basis @basis
            :javac-opts ["--release" "17"]})
  (b/write-pom {:class-dir class-dir
                :lib lib
                :version version
                :basis @basis
                :src-dirs ["runtime/src"]
                ;; 0.9.2 has no :pom-data: metadata comes from the ./pom.xml
                ;; template, coordinates/deps are synced in from here.
                })
  (b/copy-dir {:src-dirs ["runtime/src"] :target-dir class-dir})
  (b/jar {:class-dir class-dir :jar-file jar-file}))
