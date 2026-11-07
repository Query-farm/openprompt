<p align="center">
  <a href="https://query.farm">
    <picture>
      <source media="(prefers-color-scheme: dark)" srcset="https://query.farm/media-kit/logo/wordmark-dark.svg">
      <img alt="Query.Farm" src="https://query.farm/media-kit/logo/wordmark-light.svg" height="64">
    </picture>
  </a>
</p>

# Open Prompt Extension for DuckDB

[![DuckDB](https://img.shields.io/badge/DuckDB-extension-fdf1e0?logo=duckdb&logoColor=fff000)](https://query.farm/products/extensions/openprompt/)
[![v2.0 build](https://github.com/Query-farm/openprompt/actions/workflows/MainDistributionPipeline.yml/badge.svg?branch=v2.0)](https://github.com/Query-farm/openprompt/actions/workflows/MainDistributionPipeline.yml?query=branch%3Av2.0)

Prompt OpenAI-compatible LLM APIs (OpenAI, Ollama, Groq, vLLM, ...) from SQL with `open_prompt()`, including
structured JSON output, with credentials kept in DuckDB secrets.

```sql
CREATE SECRET (TYPE open_prompt, api_token 'sk-...', api_url 'https://api.openai.com/v1/chat/completions',
               model_name 'gpt-4o-mini');
SELECT open_prompt('Summarize: ' || body, max_tokens := 100) FROM articles;
```

## Documentation

Full documentation, including installation, usage, the function reference, and cookbook examples, is available at:

**[https://query.farm/products/extensions/openprompt/](https://query.farm/products/extensions/openprompt/)**
