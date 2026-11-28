import java.util.AbstractList;
import java.util.Arrays;
import java.util.List;

/**
 * Growable primitive lists behind repeated numeric fields. The generated
 * Builder appends without boxing (one {@code int[]}/{@code boolean[]} that
 * grows like an ArrayList, lazy until the first add); the built message
 * keeps one exact-size array, and the codec reads that array directly, so
 * the encode/decode loops never box. The public {@code List} getters stay
 * (protobuf-java compatible) as read-only views, used off the hot path.
 */
final class PrimLists {
    private PrimLists() {
    }

    static final int[] EMPTY_INT = {};
    static final boolean[] EMPTY_BOOL = {};

    /** Read-only {@code List} view over a snapshot array; mutators throw. */
    static List<Integer> view(int[] a) {
        if (a.length == 0) {
            return java.util.Collections.emptyList();
        }
        return new AbstractList<Integer>() {
            @Override
            public Integer get(int i) {
                return a[i];
            }

            @Override
            public int size() {
                return a.length;
            }
        };
    }

    /** Read-only {@code List} view over a snapshot array; mutators throw. */
    static List<Boolean> view(boolean[] a) {
        if (a.length == 0) {
            return java.util.Collections.emptyList();
        }
        return new AbstractList<Boolean>() {
            @Override
            public Boolean get(int i) {
                return a[i];
            }

            @Override
            public int size() {
                return a.length;
            }
        };
    }

    /** Growable {@code int} list; empty until the first add. */
    static final class Ints {
        /** Shared vacant slot; the Builder copy-on-writes on first mutation. */
        static final Ints EMPTY = new Ints();

        private int[] a;
        private int n;

        void add(int v) {
            if (a == null) {
                a = new int[8];
            } else if (n == a.length) {
                a = Arrays.copyOf(a, n * 2);
            }
            a[n++] = v;
        }

        int get(int i) {
            if (i < 0 || i >= n) {
                throw new IndexOutOfBoundsException("index " + i + ", size " + n);
            }
            return a[i];
        }

        void set(int i, int v) {
            if (i < 0 || i >= n) {
                throw new IndexOutOfBoundsException("index " + i + ", size " + n);
            }
            a[i] = v;
        }

        int size() {
            return n;
        }

        boolean isEmpty() {
            return n == 0;
        }

        /** Exact-size snapshot; the shared empty array when vacant. */
        int[] toArray() {
            if (n == 0) {
                return EMPTY_INT;
            }
            return Arrays.copyOf(a, n);
        }
    }

    /** Growable {@code boolean} list; empty until the first add. */
    static final class Bools {
        /** Shared vacant slot; the Builder copy-on-writes on first mutation. */
        static final Bools EMPTY = new Bools();

        private boolean[] a;
        private int n;

        void add(boolean v) {
            if (a == null) {
                a = new boolean[8];
            } else if (n == a.length) {
                a = Arrays.copyOf(a, n * 2);
            }
            a[n++] = v;
        }

        boolean get(int i) {
            if (i < 0 || i >= n) {
                throw new IndexOutOfBoundsException("index " + i + ", size " + n);
            }
            return a[i];
        }

        void set(int i, boolean v) {
            if (i < 0 || i >= n) {
                throw new IndexOutOfBoundsException("index " + i + ", size " + n);
            }
            a[i] = v;
        }

        int size() {
            return n;
        }

        boolean isEmpty() {
            return n == 0;
        }

        /** Exact-size snapshot; the shared empty array when vacant. */
        boolean[] toArray() {
            if (n == 0) {
                return EMPTY_BOOL;
            }
            return Arrays.copyOf(a, n);
        }
    }
}
