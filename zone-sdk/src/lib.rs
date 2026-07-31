include!("generated/provider_gen.rs");

pub struct WalletConsumerImpl;

impl WalletConsumer for WalletConsumerImpl {
    fn run_test(&mut self) {
        println!("Consumer: Requesting time info from Blockchain Module...");
        let mut blockchain = modules().blockchain_module;

        match blockchain.get_time_info() {
            Ok(info) => println!("Consumer: Success! Time info: {}", info),
            Err(e) => eprintln!("Consumer: Call failed: {:?}", e),
        }
    }
}

#[no_mangle]
pub extern "C" fn logos_module_install() -> *mut std::ffi::c_void {
    install(WalletConsumerImpl)
}
