# characters.json - Named Bot Personalities

This file assigns individual RP personalities to specific playerbot characters by name.

## Format

```json
[
    {
        "name": "ИмяПерсонажа",
        "prompt": "Описание характера на русском языке."
    }
]
```

- `name` must match the character name **exactly as it appears in game** (case-sensitive).
- `prompt` is a free-form personality description injected into the bot's LLM prompt.
- Use only ASCII `-` and `:` in Russian text — the 3.3.5a client has no glyphs for Unicode dashes.

## How it works

On worldserver startup, the module reads this file and registers each entry as a
`named:<name>` personality key in the prompt map. When a bot with a matching name
speaks, the named personality takes priority over any randomly assigned personality.

To add a new character: insert a JSON object into the array, rebuild the PTR worldserver,
and restart. No SQL migration needed.

## Config keys

```
OllamaChat.EnableNamedCharacters = 1
OllamaChat.NamedCharactersFile   = ../../../modules/mod-ollama-chat/data/characters.json
OllamaChat.EnableRPPersonalities = 1
```

All three must be 1 for named personalities to take effect.
