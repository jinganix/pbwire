//! A round trip over the pinned `matrix3.desc`: build, encode, decode, read back.
//!
//!   cargo run --example roundtrip -- ../c/tests/data/matrix3.desc

use pbwire::Schema;

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let path = std::env::args()
        .nth(1)
        .unwrap_or_else(|| "../c/tests/data/matrix3.desc".to_string());
    let schema = Schema::from_file(&path)?;
    let ty = schema.message("structtest.Scalars3")?;
    let inner_ty = schema.message("structtest.Inner3")?;

    let mut msg = ty.new_message();
    msg.set_i64(ty.field("i32")?, -42)
        .set_str(ty.field("str")?, "alice")
        .set_bool(ty.field("flag")?, true);
    for tag in ["a", "bb"] {
        msg.set_str(ty.field("tags")?, tag);
    }
    msg.set_message(ty.field("inner")?)
        .set_str(inner_ty.field("name")?, "kid")
        .set_i64(inner_ty.field("value")?, 42);

    let wire = msg.encode()?;
    println!("{} bytes: {wire:02x?}", wire.len());

    let decoded = ty.decode(&wire)?;
    println!("i32   = {}", decoded.get_i64(ty.field("i32")?));
    println!("str   = {:?}", decoded.get_str(ty.field("str")?));
    println!("inner = {:?}", match decoded.get_message(ty.field("inner")?) {
        Some(inner) => inner.get_str(inner_ty.field("name")?).map(str::to_string),
        None => None,
    });
    if let Some(tags) = decoded.value(ty.field("tags")?) {
        let tags: Vec<&str> = (0..tags.len())
            .filter_map(|i| tags.at(i)?.as_str())
            .collect();
        println!("tags  = {tags:?}");
    }
    assert_eq!(decoded.encode()?, wire, "byte-exact round trip");
    Ok(())
}
