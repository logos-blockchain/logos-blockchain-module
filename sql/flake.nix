{
  description = "Logos SQL module for Basecamp";

  inputs.blockchain-module.url = "path:..";

  outputs = { blockchain-module, ... }: blockchain-module.sql;
}
