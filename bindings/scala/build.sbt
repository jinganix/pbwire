// sbt project for the pbwire Scala binding.
//
// The Scala binding reuses the Java binding's runtime and adds only its
// DSL, so this project compiles the Java sources in place: no published
// pbwire-java jar is needed, and there is exactly one codec. The DSL
// itself is plain Scala 2.13 / 3.x (implicit classes, no version-specific
// syntax), so any recent Scala line builds it.

ThisBuild / organization := "io.github.jinganix"
ThisBuild / version := "0.1.7"
ThisBuild / scalaVersion := "3.3.6"

lazy val root = (project in file("."))
  .settings(
    name := "pbwire-scala",
    // The Java binding's runtime: the binding's only codec. Compiled from
    // source so the two stay in sync without a published artifact.
    Compile / unmanagedSourceDirectories +=
      baseDirectory.value / ".." / "java" / "runtime",
    Compile / unmanagedSourceDirectories +=
      baseDirectory.value / "runtime",
    Test / unmanagedSourceDirectories +=
      baseDirectory.value / "tests",
    // No test framework: the self-check is a plain main run against the
    // pinned oracle bytes, e.g.
    //   sbt "Test/runMain TestPbwire ../c/tests/data/matrix3.desc ../../tests/data/edge.desc"
    licenses := Seq("Apache-2.0" -> url("https://www.apache.org/licenses/LICENSE-2.0")),
    homepage := Some(url("https://github.com/jinganix/pbwire"))
  )
