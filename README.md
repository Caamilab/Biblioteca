Link para o vídeo: https://youtu.be/1sUmn0zY7PI

Inicialmente, interpretei os requisitos do projeto de forma que me levou a implementar uma solução utilizando dois semáforos de contagem para tratar separadamente os eventos de entrada e saída de usuários na biblioteca, juntamente com um mutex para proteger o contador de usuários. A motivação para essa escolha era garantir que os eventos gerados pelos botões A e B não fossem perdidos, mesmo que ocorressem de forma concorrente. No entanto, após uma reavaliação mais cuidadosa dos requisitos, percebi que o problema proposto provavelmente esperava uma solução que tratasse a concorrência de maneira mais simples, utilizando um único semáforo de contagem para regular o acesso e controlar a entrada e saída de usuários. Por esse motivo, realizei alterações no projeto, substituindo a abordagem anterior por uma nova implementação em que as tasks associadas aos botões A e B compartilham um mesmo semáforo.

[Veja os detalhes completos dessa nova estrutura (PDF)](Biblioteca/TrabalhoSE_FSA_06_Camila_De_Araujo_Bastos.pdf)

