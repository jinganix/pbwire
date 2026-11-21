package runtime_test

import (
	"os"
	"sync"
	"testing"

	"google.golang.org/protobuf/proto"
	"google.golang.org/protobuf/types/descriptorpb"
	"pbwire/runtime"
)

// svcSet builds a FileDescriptorSet with two messages and one service:
// Greeter.Hello is unary, Greeter.Chat streams both ways.
func svcSet(t *testing.T) []byte {
	t.Helper()
	str := func(s string) *string { return &s }
	i32 := func(v int32) *int32 { return &v }
	fd := &descriptorpb.FileDescriptorProto{
		Name:    str("svc.proto"),
		Package: str("svctest"),
		Syntax:  str("proto3"),
		MessageType: []*descriptorpb.DescriptorProto{
			{
				Name: str("Req"),
				Field: []*descriptorpb.FieldDescriptorProto{
					{
						Name:     str("a"),
						Number:   i32(1),
						Label:    descriptorpb.FieldDescriptorProto_LABEL_OPTIONAL.Enum(),
						Type:     descriptorpb.FieldDescriptorProto_TYPE_INT32.Enum(),
						JsonName: str("a"),
					},
				},
			},
			{
				Name: str("Resp"),
				Field: []*descriptorpb.FieldDescriptorProto{
					{
						Name:     str("b"),
						Number:   i32(1),
						Label:    descriptorpb.FieldDescriptorProto_LABEL_OPTIONAL.Enum(),
						Type:     descriptorpb.FieldDescriptorProto_TYPE_STRING.Enum(),
						JsonName: str("b"),
					},
				},
			},
		},
		Service: []*descriptorpb.ServiceDescriptorProto{
			{
				Name: str("Greeter"),
				Method: []*descriptorpb.MethodDescriptorProto{
					{
						Name:       str("Hello"),
						InputType:  str(".svctest.Req"),
						OutputType: str(".svctest.Resp"),
					},
					{
						Name:            str("Chat"),
						InputType:       str(".svctest.Req"),
						OutputType:      str(".svctest.Resp"),
						ClientStreaming: proto.Bool(true),
						ServerStreaming: proto.Bool(true),
					},
				},
			},
		},
	}
	raw, err := proto.Marshal(&descriptorpb.FileDescriptorSet{File: []*descriptorpb.FileDescriptorProto{fd}})
	if err != nil {
		t.Fatal(err)
	}
	return raw
}

func TestRegistryBasics(t *testing.T) {
	var r runtime.Registry
	if got := r.Current(); got != nil {
		t.Fatalf("Current before Swap = %v, want nil", got)
	}
	if _, err := r.Load([]byte{0xff, 0xff}); err == nil {
		t.Fatal("Load(garbage) should fail")
	}
	if got := r.Current(); got != nil {
		t.Fatalf("failed Load installed %v, want nil Current", got)
	}
	raw, err := os.ReadFile("testdata/matrix3.desc")
	if err != nil {
		t.Fatal(err)
	}
	s, err := r.Load(raw)
	if err != nil {
		t.Fatalf("Load: %v", err)
	}
	if got := r.Current(); got != nil {
		t.Fatalf("Load installed %v, want nil Current before Swap", got)
	}
	r.Swap(s)
	if got := r.Current(); got != s {
		t.Fatalf("Current = %v, want the swapped schema", got)
	}
	if _, err := s.Service("svctest.Nope"); err == nil {
		t.Error("Service on a set without services should fail")
	}
}

func TestService(t *testing.T) {
	s, err := runtime.Load(svcSet(t))
	if err != nil {
		t.Fatalf("Load: %v", err)
	}
	svc, err := s.Service("svctest.Greeter")
	if err != nil {
		t.Fatalf("Service: %v", err)
	}
	hello, err := svc.Method("Hello")
	if err != nil {
		t.Fatalf("Method(Hello): %v", err)
	}
	if hello.Input == nil || hello.Input.Plan().Full != "svctest.Req" {
		t.Errorf("Hello input = %+v, want svctest.Req", hello.Input)
	}
	if hello.Output == nil || hello.Output.Plan().Full != "svctest.Resp" {
		t.Errorf("Hello output = %+v, want svctest.Resp", hello.Output)
	}
	if hello.ClientStreaming || hello.ServerStreaming {
		t.Errorf("Hello streaming = %v/%v, want unary", hello.ClientStreaming, hello.ServerStreaming)
	}
	chat, err := svc.Method("Chat")
	if err != nil {
		t.Fatalf("Method(Chat): %v", err)
	}
	if !chat.ClientStreaming || !chat.ServerStreaming {
		t.Errorf("Chat streaming = %v/%v, want both", chat.ClientStreaming, chat.ServerStreaming)
	}
	// The resolved input type decodes real wire bytes.
	req, err := hello.Input.Decode([]byte{0x08, 0x07})
	if err != nil {
		t.Fatalf("decode through method input: %v", err)
	}
	v, ok, err := req.GetInt("a")
	if err != nil || !ok || v != 7 {
		t.Errorf("req.a = %d,%v,%v, want 7,true,nil", v, ok, err)
	}
	if _, err := svc.Method("Nope"); err == nil {
		t.Error("Method(unknown) should fail")
	}
	if _, err := s.Service("svctest.Nope"); err == nil {
		t.Error("Service(unknown) should fail")
	}
}

func TestRegistryHotSwap(t *testing.T) {
	raw1, err := os.ReadFile("testdata/matrix3.desc")
	if err != nil {
		t.Fatal(err)
	}
	raw2, err := os.ReadFile("testdata/oneof1.desc")
	if err != nil {
		t.Fatal(err)
	}
	var r runtime.Registry
	s1, err := r.Load(raw1)
	if err != nil {
		t.Fatal(err)
	}
	s2, err := r.Load(raw2)
	if err != nil {
		t.Fatal(err)
	}
	t1, err := s1.Type("structtest.Scalars3")
	if err != nil {
		t.Fatal(err)
	}
	t2, err := s2.Type("spectest.Oneof1")
	if err != nil {
		t.Fatal(err)
	}
	mk1 := func() []byte {
		m := t1.New()
		if err := m.SetInt("i32", 1); err != nil {
			t.Fatal(err)
		}
		wire, err := m.Encode()
		if err != nil {
			t.Fatal(err)
		}
		return wire
	}
	mk2 := func() []byte {
		m := t2.New()
		if err := m.SetString("name", "x"); err != nil {
			t.Fatal(err)
		}
		wire, err := m.Encode()
		if err != nil {
			t.Fatal(err)
		}
		return wire
	}
	wire1, wire2 := mk1(), mk2()
	r.Swap(s1)

	// In-flight use of a retired schema stays valid after the swap.
	old := r.Current()
	r.Swap(s2)
	if got := r.Current(); got != s2 {
		t.Fatalf("Current = %v, want s2", got)
	}
	if _, err := old.Type("structtest.Scalars3"); err != nil {
		t.Fatalf("retired schema unusable: %v", err)
	}
	ot, _ := old.Type("structtest.Scalars3")
	if _, err := ot.Decode(wire1); err != nil {
		t.Fatalf("decode with retired schema: %v", err)
	}

	var wg sync.WaitGroup
	for i := 0; i < 4; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for j := 0; j < 50; j++ {
				cur := r.Current()
				if cur == s1 {
					typ, err := cur.Type("structtest.Scalars3")
					if err != nil {
						t.Error(err)
						return
					}
					if _, err := typ.Decode(wire1); err != nil {
						t.Error(err)
						return
					}
				} else {
					typ, err := cur.Type("spectest.Oneof1")
					if err != nil {
						t.Error(err)
						return
					}
					if _, err := typ.Decode(wire2); err != nil {
						t.Error(err)
						return
					}
				}
			}
		}()
	}
	for j := 0; j < 50; j++ {
		if j%2 == 0 {
			r.Swap(s1)
		} else {
			r.Swap(s2)
		}
	}
	wg.Wait()
}
