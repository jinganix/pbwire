import java.lang.invoke.CallSite;
import java.lang.invoke.LambdaMetafactory;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.Constructor;
import java.lang.reflect.Method;
import java.util.function.BiConsumer;
import java.util.function.Function;
import java.util.function.ObjDoubleConsumer;
import java.util.function.ObjIntConsumer;
import java.util.function.ObjLongConsumer;
import java.util.function.Predicate;
import java.util.function.Supplier;

/**
 * Binds canonical Builder setters by name, once per table: the generated
 * Meta file names the method, this resolves it to a functional interface
 * with a direct call to the target (spun via LambdaMetafactory, so the
 * hot decode path pays one interface call per field, like a hand-written
 * lambda, instead of a MethodHandle.invoke adaptation per call -- a
 * runtime setter failure on the fast path is wrapped in an
 * IllegalStateException, exactly like the fallback path).
 * No
 * per-call reflection and no boxing for the int/long/double shapes, so a
 * rebound table encodes exactly like a hand-written one. A missing or
 * misshapen method fails here, loudly, instead of on the wire. Methods
 * the lookup cannot link directly (non-public targets) fall back to the
 * MethodHandle.invoke path.
 */
final class Bind {
    private Bind() {
    }

    static <B> ObjIntConsumer<B> intSetter(Class<B> builderClass, String name) {
        Method m = method(builderClass, name, int.class);
        ObjIntConsumer<B> direct = spinInt(builderClass, name, m);
        if (direct != null) {
            return direct;
        }
        MethodHandle mh = unreflect(m);
        return (b, v) -> {
            try {
                mh.invoke(b, v);
            } catch (Throwable t) {
                throw failed(builderClass, name, t);
            }
        };
    }

    static <B> ObjLongConsumer<B> longSetter(Class<B> builderClass, String name) {
        Method m = method(builderClass, name, long.class);
        ObjLongConsumer<B> direct = spinLong(builderClass, name, m);
        if (direct != null) {
            return direct;
        }
        MethodHandle mh = unreflect(m);
        return (b, v) -> {
            try {
                mh.invoke(b, v);
            } catch (Throwable t) {
                throw failed(builderClass, name, t);
            }
        };
    }

    static <B> ObjDoubleConsumer<B> doubleSetter(Class<B> builderClass, String name) {
        Method m = method(builderClass, name, double.class);
        ObjDoubleConsumer<B> direct = spinDouble(builderClass, name, m);
        if (direct != null) {
            return direct;
        }
        MethodHandle mh = unreflect(m);
        return (b, v) -> {
            try {
                mh.invoke(b, v);
            } catch (Throwable t) {
                throw failed(builderClass, name, t);
            }
        };
    }

    static <B, T> BiConsumer<B, T> setter(Class<B> builderClass, String name,
            Class<T> param) {
        Method m = method(builderClass, name, param);
        BiConsumer<B, T> direct = spinObj(builderClass, name, m, param);
        if (direct != null) {
            return direct;
        }
        MethodHandle mh = unreflect(m);
        @SuppressWarnings("unchecked")
        BiConsumer<B, T> set = (BiConsumer<B, T>) (BiConsumer<?, ?>) (b, v) -> {
            try {
                mh.invoke(b, v);
            } catch (Throwable t) {
                throw failed(builderClass, name, t);
            }
        };
        return set;
    }

    /** Same loud failure as before, factored out of the lambdas. */
    private static IllegalStateException failed(Class<?> builderClass, String name,
            Throwable t) {
        return new IllegalStateException(
                builderClass.getName() + "." + name + " failed", t);
    }

    /**
     * Spins a direct implementation of the functional interface over the
     * target method. A void SAM accepts a value-returning impl (the
     * chaining Builder is dropped), which is what makes the generated
     * setters link without an adapter. Returns null when the target is
     * not linkable from here, so the caller keeps the reflective path.
     */
    private static <B> ObjIntConsumer<B> spinInt(Class<B> builderClass, String name,
            Method m) {
        try {
            MethodHandle impl = MethodHandles.lookup().unreflect(m);
            CallSite cs = LambdaMetafactory.metafactory(MethodHandles.lookup(),
                    "accept", MethodType.methodType(ObjIntConsumer.class),
                    MethodType.methodType(void.class, Object.class, int.class),
                    impl, MethodType.methodType(void.class, builderClass, int.class));
            @SuppressWarnings("unchecked")
            ObjIntConsumer<B> direct = (ObjIntConsumer<B>) cs.getTarget().invoke();
            return (b, v) -> {
                try {
                    direct.accept(b, v);
                } catch (Throwable t) {
                    throw failed(builderClass, name, t);
                }
            };
        } catch (Throwable t) {
            return null;
        }
    }

    private static <B> ObjLongConsumer<B> spinLong(Class<B> builderClass, String name,
            Method m) {
        try {
            MethodHandle impl = MethodHandles.lookup().unreflect(m);
            CallSite cs = LambdaMetafactory.metafactory(MethodHandles.lookup(),
                    "accept", MethodType.methodType(ObjLongConsumer.class),
                    MethodType.methodType(void.class, Object.class, long.class),
                    impl, MethodType.methodType(void.class, builderClass, long.class));
            @SuppressWarnings("unchecked")
            ObjLongConsumer<B> direct = (ObjLongConsumer<B>) cs.getTarget().invoke();
            return (b, v) -> {
                try {
                    direct.accept(b, v);
                } catch (Throwable t) {
                    throw failed(builderClass, name, t);
                }
            };
        } catch (Throwable t) {
            return null;
        }
    }

    private static <B> ObjDoubleConsumer<B> spinDouble(Class<B> builderClass, String name,
            Method m) {
        try {
            MethodHandle impl = MethodHandles.lookup().unreflect(m);
            CallSite cs = LambdaMetafactory.metafactory(MethodHandles.lookup(),
                    "accept", MethodType.methodType(ObjDoubleConsumer.class),
                    MethodType.methodType(void.class, Object.class, double.class),
                    impl, MethodType.methodType(void.class, builderClass, double.class));
            @SuppressWarnings("unchecked")
            ObjDoubleConsumer<B> direct = (ObjDoubleConsumer<B>) cs.getTarget().invoke();
            return (b, v) -> {
                try {
                    direct.accept(b, v);
                } catch (Throwable t) {
                    throw failed(builderClass, name, t);
                }
            };
        } catch (Throwable t) {
            return null;
        }
    }

    private static <B, T> BiConsumer<B, T> spinObj(Class<B> builderClass, String name,
            Method m, Class<T> param) {
        try {
            MethodHandle impl = MethodHandles.lookup().unreflect(m);
            CallSite cs = LambdaMetafactory.metafactory(MethodHandles.lookup(),
                    "accept", MethodType.methodType(BiConsumer.class),
                    MethodType.methodType(void.class, Object.class, Object.class),
                    impl, MethodType.methodType(void.class, builderClass, param));
            @SuppressWarnings("unchecked")
            BiConsumer<B, T> direct = (BiConsumer<B, T>) cs.getTarget().invoke();
            return (b, v) -> {
                try {
                    direct.accept(b, v);
                } catch (Throwable t) {
                    throw failed(builderClass, name, t);
                }
            };
        } catch (Throwable t) {
            return null;
        }
    }

    static <B> Predicate<B> checker(Class<B> builderClass, String name) {
        MethodHandle mh = handle(builderClass, name);
        return b -> {
            try {
                return (boolean) mh.invoke(b);
            } catch (Throwable t) {
                throw new IllegalStateException(
                        builderClass.getName() + "." + name + " failed", t);
            }
        };
    }

    static <B> Supplier<B> factory(Class<B> builderClass) {
        final Constructor<B> ctor;
        try {
            ctor = builderClass.getDeclaredConstructor();
        } catch (NoSuchMethodException e) {
            throw new IllegalArgumentException(
                    builderClass.getName() + " needs a public no-arg constructor", e);
        }
        try {
            ctor.setAccessible(true);
        } catch (RuntimeException ignored) {
            // Already accessible; newInstance enforces the rest.
        }
        return () -> {
            try {
                return ctor.newInstance();
            } catch (ReflectiveOperationException e) {
                throw new IllegalStateException(
                        builderClass.getName() + " failed to instantiate", e);
            }
        };
    }

    static <B> Function<B, Object> finish(Class<B> builderClass) {
        final Method build;
        try {
            build = builderClass.getMethod("build");
        } catch (NoSuchMethodException e) {
            throw new IllegalArgumentException(
                    builderClass.getName() + " needs a public build()", e);
        }
        try {
            build.setAccessible(true);
        } catch (RuntimeException ignored) {
            // Already accessible; invoke enforces the rest.
        }
        return b -> {
            try {
                return build.invoke(b);
            } catch (ReflectiveOperationException e) {
                throw new IllegalStateException(
                        builderClass.getName() + ".build failed", e);
            }
        };
    }

    static void checkProduct(Class<?> builderClass, Class<?> messageClass,
            Class<?> iface) {
        Class<?> product = buildProduct(builderClass, iface);
        if (!messageClass.isAssignableFrom(product)) {
            throw new IllegalArgumentException(product.getName()
                    + " is not a " + messageClass.getName());
        }
    }

    static Class<?> buildProduct(Class<?> builderClass, Class<?> iface) {
        final Method build;
        try {
            build = builderClass.getMethod("build");
        } catch (NoSuchMethodException e) {
            throw new IllegalArgumentException(
                    builderClass.getName() + " needs a public build()", e);
        }
        Class<?> product = build.getReturnType();
        if (product == void.class || !iface.isAssignableFrom(product)) {
            throw new IllegalArgumentException(product.getName()
                    + " must implement " + iface.getName());
        }
        return product;
    }

    static <B> Class<B> nestedBuilder(Class<?> messageClass) {
        for (Class<?> nested : messageClass.getDeclaredClasses()) {
            if (nested.getSimpleName().equals("Builder")) {
                @SuppressWarnings("unchecked")
                Class<B> builder = (Class<B>) nested;
                return builder;
            }
        }
        throw new IllegalArgumentException(
                messageClass.getName() + " has no nested Builder class");
    }

    /** Resolves the setter, accepting the boxed spelling like before. */
    private static Method method(Class<?> builderClass, String name,
            Class<?>... params) {
        Method m = find(builderClass, name, params);
        if (m == null) {
            // Generated Builders take primitives; hand-written ones may
            // take the boxed spelling. Accept either, fail otherwise.
            Class<?>[] alt = counterpart(params);
            if (alt != null) {
                m = find(builderClass, name, alt);
            }
        }
        if (m == null) {
            throw new IllegalArgumentException(
                    builderClass.getName() + " needs a public " + name + describe(params));
        }
        return m;
    }

    private static MethodHandle unreflect(Method m) {
        try {
            m.setAccessible(true);
        } catch (RuntimeException ignored) {
            // Already accessible; unreflect enforces the rest.
        }
        try {
            return MethodHandles.lookup().unreflect(m);
        } catch (IllegalAccessException e) {
            throw new IllegalArgumentException(
                    m.getDeclaringClass().getName() + "." + m.getName()
                            + " is not accessible", e);
        }
    }

    private static MethodHandle handle(Class<?> builderClass, String name,
            Class<?>... params) {
        return unreflect(method(builderClass, name, params));
    }

    private static Method find(Class<?> builderClass, String name, Class<?>[] params) {
        try {
            return builderClass.getMethod(name, params);
        } catch (NoSuchMethodException e) {
            return null;
        }
    }

    private static Class<?>[] counterpart(Class<?>[] params) {
        if (params.length != 1) {
            return null;
        }
        Class<?> p = params[0];
        if (p == Float.class) {
            return new Class<?>[] {float.class};
        }
        if (p == float.class) {
            return new Class<?>[] {Float.class};
        }
        if (p == Boolean.class) {
            return new Class<?>[] {boolean.class};
        }
        if (p == boolean.class) {
            return new Class<?>[] {Boolean.class};
        }
        if (p == Integer.class) {
            return new Class<?>[] {int.class};
        }
        if (p == int.class) {
            return new Class<?>[] {Integer.class};
        }
        if (p == Long.class) {
            return new Class<?>[] {long.class};
        }
        if (p == long.class) {
            return new Class<?>[] {Long.class};
        }
        if (p == Double.class) {
            return new Class<?>[] {double.class};
        }
        if (p == double.class) {
            return new Class<?>[] {Double.class};
        }
        if (p == Byte.class) {
            return new Class<?>[] {byte.class};
        }
        if (p == byte.class) {
            return new Class<?>[] {Byte.class};
        }
        if (p == Short.class) {
            return new Class<?>[] {short.class};
        }
        if (p == short.class) {
            return new Class<?>[] {Short.class};
        }
        if (p == Character.class) {
            return new Class<?>[] {char.class};
        }
        if (p == char.class) {
            return new Class<?>[] {Character.class};
        }
        return null;
    }

    private static String describe(Class<?>[] params) {
        StringBuilder want = new StringBuilder("(");
        for (int i = 0; i < params.length; i++) {
            if (i > 0) {
                want.append(',');
            }
            want.append(params[i].getSimpleName());
        }
        return want.append(')').toString();
    }
}
