mod generated;

use std::thread;
use std::time::Duration;

use crate::generated::client_gen::BlockchainModuleClient;

fn main() -> Result<(), Box<dyn std::error::Error>> {
    println!("Starting logos-zone-module client...");

    let mut client = BlockchainModuleClient::new();

    println!("Fetching time info...");
    match client.get_time_info() {
        Ok(time_json) => println!("Success! Time info: {}", time_json),
        Err(e) => eprintln!("Failed to fetch time info: {:?}", e),
    }

    let event_subscription = client.on_new_block()?;

    thread::spawn(move || {
        println!("Listening for newBlock events...");

        for raw_event in event_subscription {
            if let Some(new_block) = BlockchainModuleClient::decode_new_block(&raw_event) {
                println!("New block received! JSON payload: {}", new_block.block_json);
            } else {
                eprintln!("Received malformed newBlock event.");
            }
        }
        println!("Event stream disconnected.");
    });

    client.wallet_get_known_addresses_async(|result| match result {
        Ok(addresses) => println!("Known addresses: {}", addresses),
        Err(e) => eprintln!("Failed to get addresses: {:?}", e),
    });

    loop {
        thread::sleep(Duration::from_secs(1));
    }
}
