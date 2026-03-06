fn main() {
    cc::Build::new()
        .file("../core/residc.c")
        .file("../sdk/residc_sdk.c")
        .include("../core")
        .include("../sdk")
        .opt_level(2)
        .warnings(false)
        .compile("resdc");
}
