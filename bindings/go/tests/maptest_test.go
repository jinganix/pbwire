package tests

// Map coverage for the generated model: maps ride as repeated entry structs
// in every language (TESTSPEC T1+T2 for the map shape). The reference bytes
// are codegen/testdata/maptest.bin (protoc --encode of maptest.txt); entry
// order on the wire is the array order, like the C struct binding.
import (
	"bytes"
	"os"
	"testing"

	genmap "pbwire/tests/gen/maptest"
)

func TestMapBytes(t *testing.T) {
	ref, err := os.ReadFile(testdata(t, "maptest.bin"))
	if err != nil {
		t.Fatal(err)
	}
	msg := &genmap.MapTest{
		ByInt: []*genmap.MapTest_ByIntEntry{
			{Key: 1, Value: "one"},
			{Key: -5, Value: "neg"},
		},
		ByStr: []*genmap.MapTest_ByStrEntry{
			{Key: "a", Value: 10},
			{Key: "b", Value: -3},
		},
		ByMsg: []*genmap.MapTest_ByMsgEntry{
			{Key: "k", Value: &genmap.MapValue{Name: "n", Value: 42}},
		},
	}
	wire, err := msg.Marshal()
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(wire, ref) {
		t.Fatal("generated map Marshal != protoc reference bytes")
	}
	back := &genmap.MapTest{}
	if err := back.Unmarshal(ref); err != nil {
		t.Fatal(err)
	}
	if len(back.ByInt) != 2 || back.ByInt[0].Key != 1 ||
		back.ByInt[0].Value != "one" || back.ByInt[1].Key != -5 ||
		back.ByInt[1].Value != "neg" {
		t.Fatalf("by_int mismatch: %+v", back.ByInt)
	}
	if len(back.ByStr) != 2 || back.ByStr[0].Key != "a" ||
		back.ByStr[0].Value != 10 || back.ByStr[1].Key != "b" ||
		back.ByStr[1].Value != -3 {
		t.Fatalf("by_str mismatch: %+v", back.ByStr)
	}
	if len(back.ByMsg) != 1 || back.ByMsg[0].Key != "k" ||
		back.ByMsg[0].Value.GetName() != "n" ||
		back.ByMsg[0].Value.GetValue() != 42 {
		t.Fatalf("by_msg mismatch: %+v", back.ByMsg)
	}
	again, err := back.Marshal()
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(again, ref) {
		t.Fatal("decode/encode of the map reference is not byte-exact")
	}
	empty, err := (&genmap.MapTest{}).Marshal()
	if err != nil || len(empty) != 0 {
		t.Fatal("empty map message must encode empty")
	}
}
