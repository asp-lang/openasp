<%
Set source = Server.CreateObject("ADODB.Stream")
With source
    .Type = 2
    .Open
    .WriteText .LoadFromFile(Server.MapPath("component_stream_copy.asp"))
    .Position = 3
End With

Set target = Server.CreateObject("ADODB.Stream")
With target
    .Type = 1
    .Open
End With

source.CopyTo target
Response.Write source.Position = source.Size
Response.Write ":"
Response.Write target.Position = target.Size
Response.Write ":"
Response.Write target.Size > 0
%>
