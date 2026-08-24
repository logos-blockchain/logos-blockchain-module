fn main() {
    println!("cargo:rerun-if-changed=../demo_module.lidl");
    let out_dir = std::env::var("OUT_DIR").unwrap();
    let dest_path = std::path::Path::new(&out_dir).join("generated");
    std::fs::create_dir_all(&dest_path).unwrap();
    std::fs::copy("src/generated/provider_gen.rs", dest_path.join("provider_gen.rs")).unwrap();
}
