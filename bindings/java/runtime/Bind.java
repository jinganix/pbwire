import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
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
 * over a MethodHandle. No per-call reflection and no boxing for the
 * int/long/double shapes, so a rebound table encodes exactly like a
 * hand-written one. A missing or misshapen method fails here, loudly,
 * instead of on the wire.
 */
final class Bind {
    private Bind() {
    }

    static <B> ObjIntConsumer<B> intSetter(Class<B> builderClass, String name) {
        MethodHandle mh = handle(builderClass, name, int.class);
        return (b, v) -> {
            try {
                mh.invoke(b, v);
            } catch (Throwable t) {
                throw new IllegalStateException(
                        builderClass.getName() + "." + name + " failed", t);
            }
        };
    }

    static <B> ObjLongConsumer<B> longSetter(Class<B> builderClass, String name) {
        MethodHandle mh = handle(builderClass, name, long.class);
        return (b, v) -> {
            try {
                mh.invoke(b, v);
            } catch (Throwable t) {
                throw new IllegalStateException(
                        builderClass.getName() + "." + name + " failed", t);
            }
        };
    }

    static <B> ObjDoubleConsumer<B> doubleSetter(Class<B> builderClass, String name) {
        MethodHandle mh = handle(builderClass, name, double.class);
        return (b, v) -> {
            try {
                mh.invoke(b, v);
            } catch (Throwable t) {
                throw new IllegalStateException(
                        builderClass.getName() + "." + name + " failed", t);
            }
        };
    }

    static <B, T> BiConsumer<B, T> setter(Class<B> builderClass, String name,
            Class<T> param) {
        MethodHandle mh = handle(builderClass, name, param);
        @SuppressWarnings("unchecked")
        BiConsumer<B, T> set = (BiConsumer<B, T>) (BiConsumer<?, ?>) (b, v) -> {
            try {
                mh.invoke(b, v);
            } catch (Throwable t) {
                throw new IllegalStateException(
                        builderClass.getName() + "." + name + " failed", t);
            }
        };
        return set;
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

    private static MethodHandle handle(Class<?> builderClass, String name,
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
        try {
            m.setAccessible(true);
        } catch (RuntimeException ignored) {
            // Already accessible; unreflect enforces the rest.
        }
        try {
            return MethodHandles.lookup().unreflect(m);
        } catch (IllegalAccessException e) {
            throw new IllegalArgumentException(
                    builderClass.getName() + "." + name + " is not accessible", e);
        }
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
