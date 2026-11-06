import java.util.Arrays;

/**
 * A message's compiled table: field rows plus a dense number-to-row map.
 * Generated once per message; the engine reads it, never a descriptor set.
 */
final class Meta<O, B> {
    final String fullName;
    final FieldMeta<O, B>[] fields;
    // Oneof names in field order; FieldMeta.oneof indexes into this.
    // Empty when the message has no oneof.
    final String[] oneofNames;
    final int[] slotByFnum;
    final boolean hasRequired; // some row is proto2 required

    Meta(String fullName, FieldMeta<O, B>[] fields) {
        this(fullName, fields, new String[0]);
    }

    Meta(String fullName, FieldMeta<O, B>[] fields, String[] oneofNames) {
        this.fullName = fullName;
        this.fields = fields;
        this.oneofNames = oneofNames;
        int max = 0;
        boolean req = false;
        for (FieldMeta<O, B> f : fields) {
            max = Math.max(max, f.number);
            req = req || f.required;
        }
        slotByFnum = new int[max + 1];
        Arrays.fill(slotByFnum, -1);
        for (int i = 0; i < fields.length; i++) {
            slotByFnum[fields[i].number] = i;
        }
        hasRequired = req;
    }

    int slot(int fnum) {
        return fnum < slotByFnum.length ? slotByFnum[fnum] : -1;
    }
}
