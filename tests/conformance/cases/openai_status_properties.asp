<%
Dim ai
Dim body

Set ai = Server.CreateObject("OpenASP.OpenAI")
ai.BaseURL = "http://127.0.0.1:" & Request("port") & "/v1"
ai.APIKey = "test-secret"
ai.Model = "mock-model"
body = ai.ChatCompletions("hello")
Response.Write CStr(ai.Status) & "|" & body & "|" & ai.LastResponse
%>
