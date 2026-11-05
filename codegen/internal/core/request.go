package core

import (
	"strings"

	"google.golang.org/protobuf/reflect/protodesc"
	"google.golang.org/protobuf/reflect/protoreflect"
	"google.golang.org/protobuf/reflect/protoregistry"
	pluginpb "google.golang.org/protobuf/types/pluginpb"
)

func normalizeProtoPath(path string) string {
	return strings.ReplaceAll(path, "\\", "/")
}

type filesResolver struct {
	files map[string]protoreflect.FileDescriptor
}

func (r *filesResolver) FindFileByPath(path string) (protoreflect.FileDescriptor, error) {
	if fd, ok := r.files[normalizeProtoPath(path)]; ok {
		return fd, nil
	}
	return nil, protoregistry.NotFound
}

func (r *filesResolver) FindDescriptorByName(name protoreflect.FullName) (protoreflect.Descriptor, error) {
	// protoc sends files in topological order (imports first), so a
	// cross-file message/enum reference resolves against an already built
	// file. Without this every foreign type stays an unresolvable
	// placeholder and the model must skip the field.
	s := strings.TrimPrefix(string(name), ".")
	for _, fd := range r.files {
		pkg := string(fd.Package())
		var rest string
		if pkg == "" {
			rest = s
		} else if r, ok := strings.CutPrefix(s, pkg+"."); ok {
			rest = r
		} else {
			continue
		}
		if d := findInFile(fd, strings.Split(rest, ".")); d != nil {
			return d, nil
		}
	}
	return nil, protoregistry.NotFound
}

func findInFile(fd protoreflect.FileDescriptor, parts []string) protoreflect.Descriptor {
	if len(parts) == 0 {
		return nil
	}
	msgs := fd.Messages()
	if m := msgs.ByName(protoreflect.Name(parts[0])); m != nil {
		if len(parts) == 1 {
			return m
		}
		return findInMessage(m, parts[1:])
	}
	if e := fd.Enums().ByName(protoreflect.Name(parts[0])); e != nil && len(parts) == 1 {
		return e
	}
	return nil
}

func findInMessage(md protoreflect.MessageDescriptor, parts []string) protoreflect.Descriptor {
	if len(parts) == 0 {
		return md
	}
	if m := md.Messages().ByName(protoreflect.Name(parts[0])); m != nil {
		if len(parts) == 1 {
			return m
		}
		return findInMessage(m, parts[1:])
	}
	if e := md.Enums().ByName(protoreflect.Name(parts[0])); e != nil && len(parts) == 1 {
		return e
	}
	return nil
}

// RequestContext holds parsed file descriptors from a code generator request.
type RequestContext struct {
	Descriptors       []protoreflect.FileDescriptor
	TargetDescriptors []protoreflect.FileDescriptor
}

// NewRequestContext builds descriptors without extension registry.
func NewRequestContext(req *pluginpb.CodeGeneratorRequest) (*RequestContext, error) {
	all, targets, err := BuildDescriptors(req)
	if err != nil {
		return nil, err
	}
	return &RequestContext{
		Descriptors:       all,
		TargetDescriptors: targets,
	}, nil
}

// BuildDescriptors constructs file descriptors from a plugin request.
func BuildDescriptors(req *pluginpb.CodeGeneratorRequest) ([]protoreflect.FileDescriptor, []protoreflect.FileDescriptor, error) {
	filesMap := make(map[string]protoreflect.FileDescriptor)
	resolver := &filesResolver{files: filesMap}
	var all []protoreflect.FileDescriptor

	for _, pb := range req.ProtoFile {
		name := normalizeProtoPath(pb.GetName())
		pb.Name = &name
		fd, err := protodesc.FileOptions{AllowUnresolvable: true}.New(pb, resolver)
		if err != nil {
			return nil, nil, err
		}
		filesMap[name] = fd
		all = append(all, fd)
	}

	var targets []protoreflect.FileDescriptor
	for _, name := range req.FileToGenerate {
		targets = append(targets, filesMap[normalizeProtoPath(name)])
	}
	return all, targets, nil
}
