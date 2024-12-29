<img src="https://github.com/user-attachments/assets/46a5c546-7e9b-42c7-87f4-bc8defe674e0" width=250 />

# DuckDB Open Prompt Extension
Simple extension to query OpenAI Completion API endpoints such as Ollama/OpenAI/etc

> Experimental: USE AT YOUR OWN RISK!

## Installing and Loading
```sql
INSTALL open_prompt FROM community;
LOAD open_prompt;
```

### Functions
- `open_prompt(prompt)`
- `set_api_url(/v1/chat/completions)`
- `set_api_token(optional_auth_token)`
- `set_model_name(model_name)`

#### Requirements

- DuckDB v1.1.1 or higher
- API must support `/v1/chat/completions`

### Settings
Setup the completions API configuration w/ optional auth token and model name
```sql
SET VARIABLE openprompt_api_url = 'http://localhost:11434/v1/chat/completions';
SET VARIABLE openprompt_api_token = 'your_api_key_here';
SET VARIABLE openprompt_model_name = 'qwen2.5:0.5b';
```

For persistent usage, configure parameters using DuckDB `SECRETS`
```
CREATE SECRET 'openprompt_api_url' 'open_prompt' AS 'open_prompt' ('url'='http://localhost:11434/v1/chat/completions');
CREATE SECRET 'openprompt_api_token' 'open_prompt' AS 'open_prompt' ('token'='your_api_token');
CREATE SECRET 'openprompt_model_name' 'open_prompt' AS 'open_prompt' ('name'='qwen2.5:0.5b');
CREATE SECRET 'openprompt_api_timeout' 'open_prompt' AS 'open_prompt' ('timeout'='20');
```

### Usage
```sql
D SELECT open_prompt('Write a one-line poem about ducks') AS response;
┌────────────────────────────────────────────────┐
│                    response                    │
│                    varchar                     │
├────────────────────────────────────────────────┤
│ Ducks quacking at dawn, swimming in the light. │
└────────────────────────────────────────────────┘
```

#### JSON Structured Output
For supported models you can request structured JSON output by providing a schema

```sql
SET VARIABLE openprompt_api_url = 'http://localhost:11434/v1/chat/completions';
SET VARIABLE openprompt_api_token = 'your_api_key_here';
SET VARIABLE openprompt_model_name = 'llama3.2:3b';

SELECT open_prompt('I want ice cream', json_schema := '{
       "type": "object",
       "properties": {
         "summary": { "type": "string" },
         "sentiment": { "type": "string", "enum": ["pos", "neg", "neutral"] }
       },
       "required": ["summary", "sentiment"],
       "additionalProperties": false
     }');
```

For smaller models the `system_prompt` can be used to request JSON schema in _best-effort_ mode

```sql
SET VARIABLE openprompt_api_url = 'http://localhost:11434/v1/chat/completions';
SET VARIABLE openprompt_api_token = 'your_api_key_here';
SET VARIABLE openprompt_model_name = 'qwen2.5:1.5b';
SELECT open_prompt('I want ice cream.',
       system_prompt:='The respose MUST be a JSON with the following schema: {
           "type": "object",
           "properties": {
             "summary": { "type": "string" },
             "sentiment": { "type": "string", "enum": ["pos", "neg", "neutral"] }
           },
           "required": ["summary", "sentiment"],
           "additionalProperties": false
       }');
```


<br>

<img src="https://github.com/user-attachments/assets/824bfab2-aca6-4bd9-8a4a-bc01901fcd5b" width=100 />

### Ollama self-hosted
Test the open_prompt extension using a local or remote Ollama with Completions API

#### CPU only
```
docker run -d -v ollama:/root/.ollama -p 11434:11434 --name ollama ollama/ollama
```
#### Nvidia GPU
Install the Nvidia container toolkit. Run Ollama inside a Docker container
```
docker run -d --gpus=all -v ollama:/root/.ollama -p 11434:11434 --name ollama ollama/ollama
```

<br>

###### Disclaimers

* Configuration and formats loosely inspired by the [Motherduck prompt()](https://motherduck.com/docs/sql-reference/motherduck-sql-reference/ai-functions/prompt/)

> DuckDB ® is a trademark of DuckDB Foundation. Motherduck ® is a trademark of the Motherduck Corporation. Any trademarks, service marks, and logos mentioned or depicted are the property of their respective owners. The use of any third-party trademarks, brand names, product names, and company names is purely informative or intended as parody and does not imply endorsement, affiliation, or association with the respective owners.
