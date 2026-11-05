;; The C example (examples/addressbook.c), in Clojure, through the generated
;; maps: assign fields, encode, decode, compare. The encoded bytes must equal
;; the pinned reference examples/addressbook.bin.
;;
;;   java -cp <jars>:../../java/out/classes:../runtime/src:gen:. \
;;     clojure.main -m roundtrip [addressbook.bin]
(ns roundtrip
  (:require [clojure.java.io :as io]
            [addressbook :as ab])
  (:import (java.nio.file Files)
           (java.util Arrays)))

(defn phone [number kind]
  (ab/tutorial-person-phonenumber :number number :type kind))

(defn build []
  (ab/tutorial-addressbook :people
    [(ab/tutorial-person :name "Alice" :id 1
       :phones [(phone "555-0100" ab/tutorial-person-phonetype-MOBILE)])
     (ab/tutorial-person :name "Bob" :id 2 :email "bob@example.com"
       :phones [(phone "555-0200" ab/tutorial-person-phonetype-HOME)])]))

(defn -main [& args]
  (let [bin (or (first args) "../../examples/addressbook.bin")
        wire (ab/encode-tutorial-addressbook (build))]
    (when (.exists (io/file bin))
      (let [reference (Files/readAllBytes (.toPath (io/file bin)))]
        (when-not (Arrays/equals ^bytes reference ^bytes wire)
          (println (str "MISMATCH: encode differs from " bin))
          (System/exit 1))))
    (let [back (ab/decode-tutorial-addressbook wire)]
      (println (str "addressbook: " (count wire) " bytes, "
                    (count (:people back)) " people"))
      (doseq [p (:people back)]
        (println (str "  " (:name p) " (" (:id p) ") " (or (:email p) "")))
        (doseq [ph (:phones p)]
          (println (str "    " (:number ph))))))))
