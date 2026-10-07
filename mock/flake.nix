{
  description = "blockchain_module with a simulated node. See README.md.";

  # Swap it in for the real module from any consumer:
  #   --override-input blockchain_module "path:<this repo>?dir=mock"
  inputs.blockchain-module.url = "path:..";

  outputs = { blockchain-module, ... }: blockchain-module.mock;
}
