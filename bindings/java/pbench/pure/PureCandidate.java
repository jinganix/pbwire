/*
 * pbench candidate: the shipped pure-Java codec, unchanged. This directory
 * holds only the adapter that puts runtime/ behind the common pbench contract;
 * copying the codec here would let the copy drift from what actually ships.
 */
public final class PureCandidate implements Compare.Candidate {
    private final GenericCodec.MessageDef def;
    private GenericCodec.DMsg tree;

    public PureCandidate(GenericCodec.MessageDef def) {
        this.def = def;
    }

    @Override
    public String name() {
        return "pure-java";
    }

    /** encode() re-encodes the caller's tree, the same state a user holds. */
    @Override
    public void prepare(int tier, GenericCodec.DMsg model, byte[] wire) {
        tree = model;
    }

    @Override
    public byte[] encode() {
        return GenericCodec.encode(tree);
    }

    /** Decodes into a fresh tree and reads its arrays, so the work cannot be eliminated. */
    @Override
    public int decode(byte[] wire) {
        GenericCodec.DMsg d = GenericCodec.decode(def, wire);
        return d.has.length + d.num.length + (int) (d.num[0] >>> 63);
    }
}
