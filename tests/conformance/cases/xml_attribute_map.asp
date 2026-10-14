<%
Dim document, row, attribute
Set document = Server.CreateObject("MSXML2.FreeThreadedDOMDocument.3.0")
document.appendChild document.createElement("rows")
Set row = document.createNode(1, "row", "")
Set attribute = row.attributes.setNamedItem(document.createNode(2, "title", ""))
attribute.text = "first"
document.documentElement.appendChild row
Response.Write document.documentElement.selectNodes("row").Item(0).xml
%>
