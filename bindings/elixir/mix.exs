defmodule Pbwire.MixProject do
  use Mix.Project

  def project do
    [
      app: :pbwire_elixir,
      version: "0.1.9",
      elixir: "~> 1.17",
      start_permanent: Mix.env() == :prod,
      deps: deps(),
      description:
        "Elixir wrapper over the pbwire NIF: elixir-protobuf structs, C-core wire speed",
      package: package(),
      elixirc_paths: elixirc_paths(Mix.env())
    ]
  end

  def application do
    [extra_applications: [:logger]]
  end

  defp deps do
    [
      # The NIF itself: same C core as bindings/erlang, same :pbwire API.
      # The Elixir side only translates schemas and structs, it owns no wire code.
      {:pbwire, "~> 0.1"},
      # The schema plugin, like gpb is for the Erlang binding: the generated
      # structs plus __message_props__/0 that Pbwire.Schema consumes.
      {:protobuf, "~> 0.17"}
    ]
  end

  defp package do
    [licenses: ["Apache-2.0"], links: %{"GitHub" => "https://github.com/jinganix/pbwire"}]
  end

  defp elixirc_paths(:test), do: ["lib", "test/support", "test/gen"]
  defp elixirc_paths(_), do: ["lib"]
end
