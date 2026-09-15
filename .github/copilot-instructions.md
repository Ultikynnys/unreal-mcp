
# Guidelines for using Python for MCP Tools

The following guidelines apply to any method or function marked with the @mcp.tool() decorator.

- Parameters should not have any of the following types: `Any`, `object`, `Optional[T]`, `Union[T]`.
- For a given parameter `x` of type `T` that has a default value, do not use type `x : T | None = None`. Instead, use `x: T = None` and handle defaults within the method body itself.
- Always include method docstrings and make sure to given proper examples of valid inputs especially when no type hints are present.

When this rule is applied, please remember to explicitly mention it.


# NEVER bypass the Unreal MCP tools

- The ONLY supported way to drive the Unreal Editor is the `unreal_mcp_*` tools. Use them.
- NEVER open a raw socket to `127.0.0.1:55557`. NEVER copy `Python/editor/archive_mcp_client.py` or `Python/scripts/**` into a private `cmd()` helper, and NEVER stand up a client to route around a tool.
- The TCP bridge is unauthenticated on loopback by design, so a hand-rolled client is technically possible: that is exactly why this rule exists. Such a client diverges from the tool contract, skips the MCP layer's validation and logging, and breaks the moment the backend changes.
- The sanctioned path carries a `CONTROL_PLANE_SECRET` defined in `Python/unreal_mcp_server.py`. Do not lift it into a script, and do not reimplement the client around it.
- If the MCP tools are not good enough (a missing command, wrong parameters, flaky responses), YOU MUST NOT work around them. STOP and escalate to the user with the exact command and inputs you needed. The user can extend the tool surface. A workaround is never the answer.


# Guidelines for using Python for MCP Tools

The following guidelines apply to any method or function marked with the @mcp.tool() decorator.

- Parameters should not have any of the following types: `Any`, `object`, `Optional[T]`, `Union[T]`.
- For a given parameter `x` of type `T` that has a default value, do not use type `x : T | None = None`. Instead, use `x: T = None` and handle defaults within the method body itself.
- Always include method docstrings and make sure to given proper examples of valid inputs especially when no type hints are present.

When this rule is applied, please remember to explicitly mention it.