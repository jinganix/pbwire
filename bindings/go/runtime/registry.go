// Hot-swap schema registry: load new descriptor sets in the background and
// publish them with one atomic store. Schemas are immutable after Load
// returns, so the swap is an atomic pointer and retired schemas stay alive
// for in-flight use through the caller's own reference (Go-idiomatic: no
// refcount, the collector frees a schema once nobody holds it). The zero
// value is ready; Current is nil until the first Swap. Safe for concurrent
// use: many goroutines may hold Current while another swaps.
package runtime

import "sync/atomic"

// Registry publishes immutable schemas to concurrent readers.
type Registry struct {
	cur atomic.Pointer[Schema]
}

// Load parses a FileDescriptorSet into a new schema without publishing it:
// pair with Swap for load-then-swap. A failed Load installs nothing.
func (r *Registry) Load(set []byte) (*Schema, error) {
	return Load(set)
}

// Swap publishes s as the current schema. The previous schema stays valid
// for readers already holding it. Swap(nil) clears the current schema.
func (r *Registry) Swap(s *Schema) {
	r.cur.Store(s)
}

// Current returns the published schema, or nil before the first Swap.
func (r *Registry) Current() *Schema {
	return r.cur.Load()
}
