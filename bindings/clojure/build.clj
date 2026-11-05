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
                :pom-data [[:description "Dynamic protobuf over the pbwire C ABI, schema at runtime, no generated code"]
                           [:url "https://github.com/jinganix/pbwire"]
                           [:licenses
                            [:license
                             [:name "Apache-2.0"]
                             [:url "https://www.apache.org/licenses/LICENSE-2.0"]]]
                           [:developers
                            [:developer
                             [:id "gan.jin"]
                             [:name "JinGan"]
                             [:email "jinganix@gmail.com"]]]
                           [:scm
                            [:url "https://github.com/jinganix/pbwire"]
                            [:connection "scm:git:git://github.com/jinganix/pbwire.git"]
                            [:developerConnection "scm:git:ssh://github.com/jinganix/pbwire.git"]]]})
  (b/copy-dir {:src-dirs ["runtime/src"] :target-dir class-dir})
  (b/jar {:class-dir class-dir :jar-file jar-file})
  ;; deps-deploy reads artifact name/version from ./pom.xml, so leave a copy
  ;; at the project root (build output, gitignored).
  (b/copy-file {:src (b/pom-path {:lib lib :class-dir class-dir})
                :target "pom.xml"}))
