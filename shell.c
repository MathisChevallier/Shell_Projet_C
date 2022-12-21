/************************************************************************************************************
 *                                     Polytech Paris-Saclay - ET3 - Juin 2022
 * 
 * @name Computer systems course project - creation of a simplified shell
 * 
 * @file shell.c
 * 
 * @authors MARTIN HUGUES & CHEVALLIER Mathis
 * 
 * @brief The main goal of this project is to create a simplified shell, based on the operation of jobs and processes.
 *        It supports basic commands such as ./, cd, ls, cp, man, cat, ect...
 *        It manages the redirections of inputs and outputs.
 *        It manages the foreground or background of the processes, by adding ' &' at the end of the command.
 * 
 * @version 0.1
 * @date 2022-06-20
 * @copyright Copyright (c) 2022
 * 
 ************************************************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <iostream>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>
#include <signal.h> 
#include <string.h>
#include <sys/wait.h>

#define MAX_LENGTH 1024

/* A process is a single process.  */
typedef struct process
{
  struct process *next;       /* next process in pipeline */
  char **argv;                /* for exec */
  pid_t pid;                  /* process ID */
  char completed;             /* true if process has completed */
  char stopped;               /* true if process has stopped */
  int status;                 /* reported status value */
} process;

/* A job is a pipeline of processes.  */
typedef struct job
{
  struct job *next;           /* next active job */
  char *command;              /* command line, used for messages */
  process *first_process;     /* list of processes in this job */
  pid_t pgid;                 /* process group ID */
  char notified;              /* true if user told about stopped job */
  struct termios tmodes;      /* saved terminal modes */
  int stdin, stdout, stderr;  /* standard i/o channels */
} job;

/* The active jobs are linked into a list.  This is its head.   */
job *first_job = NULL;

pid_t shell_pgid;
struct termios shell_tmodes;
int shell_terminal;
int shell_is_interactive;

/*Initialisation of a process*/
process * init_process(char **commande){
  process *p = (process*) malloc(sizeof(process)); //proces's space memory
  p->next = NULL;
  p->argv = commande;
  return p;
}

/*Initialisation of a job*/
job * init_job(char *imput, char **commande ){
  job *j = (job*) malloc(sizeof(job)); //job's space memory
  j->next = NULL;
  j->command = imput;
  j->first_process = init_process(commande);
  return j;
}

/* Free job's memory space. */
void free_job(job *j){
  free(j);
}

void init_shell (){

  /* See if we are running interactively.  */
  shell_terminal = STDIN_FILENO;
  shell_is_interactive = isatty (shell_terminal);

  if (shell_is_interactive)
    {
      /* Loop until we are in the foreground.  */
      while (tcgetpgrp (shell_terminal) != (shell_pgid = getpgrp ()))
        kill (- shell_pgid, SIGTTIN);

      /* Ignore interactive and job-control signals.  */
      
      signal (SIGINT, SIG_IGN);
      signal (SIGQUIT, SIG_IGN);
      signal (SIGTSTP, SIG_IGN);
      signal (SIGTTIN, SIG_IGN);
      signal (SIGTTOU, SIG_IGN);
      signal (SIGCHLD, SIG_IGN);
    
      /* Put ourselves in our own process group.  */
      shell_pgid = getpid ();
      if (setpgid (shell_pgid, shell_pgid) < 0)
        {
          perror ("Couldn't put the shell in its own process group");
          exit (1);
        }

      /* Grab control of the terminal.  */
      tcsetpgrp (shell_terminal, shell_pgid);

      /* Save default terminal attributes for shell.  */
      tcgetattr (shell_terminal, &shell_tmodes);
    }
}

void format_job_info (job *j, const char *status)
{
  fprintf (stderr, "%ld (%s): %s\n", (long)j->pgid, status, j->command);
}

int job_is_stopped (job *j)
{
  process *p;
  for (p = j->first_process; p; p = p->next)
    if (!p->completed && !p->stopped)
      return 0;
  return 1;
}

/* Return true if all processes in the job have completed.  */
int job_is_completed (job *j)
{
  process *p;

  for (p = j->first_process; p; p = p->next)
    if (!p->completed)
      return 0;
  return 1;
}

/* Store the status of the process pid that was returned by waitpid.
Return 0 if all went well, nonzero otherwise.  */
int mark_process_status (pid_t pid, int status)
{
  job *j;
  process *p;

  if (pid > 0)
    {
      /* Update the record for the process.  */
      for (j = first_job; j; j = j->next)
        for (p = j->first_process; p; p = p->next)
          if (p->pid == pid)
            {
              p->status = status;
              if (WIFSTOPPED (status))
                p->stopped = 1;
              else
                {
                  p->completed = 1;
                  if (WIFSIGNALED (status))
                    fprintf (stderr, "%d: Terminated by signal %d.\n", (int) pid, WTERMSIG (p->status));
                }
              return 0;
             }
      fprintf (stderr, "No child process %d.\n", pid);
      return -1;
    }
  else if (pid == 0 || errno == ECHILD)
    /* No processes ready to report.  */
    return -1;
  else {
    /* Other weird errors.  */
    perror ("waitpid");
    return -1;
  }
}

void wait_for_job (job *j)
{
 int status;
 pid_t pid;

 do
  pid = waitpid (WAIT_ANY, &status, WUNTRACED);
 while (!mark_process_status (pid, status) && !job_is_stopped (j) && !job_is_completed (j));
}

/* Put job j in the foreground.  If cont is nonzero, restore the saved terminal modes and send the process group a
   SIGCONT signal to wake it up before we block.  */
void put_job_in_foreground (job *j, int cont){
  /* Put the job into the foreground.  */
  tcsetpgrp (shell_terminal, j->pgid);


  /* Send the job a continue signal, if necessary.  */
  if (cont)
    {
      tcsetattr (shell_terminal, TCSADRAIN, &j->tmodes);
      if (kill (- j->pgid, SIGCONT) < 0)
        perror ("kill (SIGCONT)");
    }

  /* Wait for it to report.  */
  wait_for_job (j);

  /* Put the shell back in the foreground.  */
  tcsetpgrp (shell_terminal, shell_pgid);

  /* Restore the shell’s terminal modes.  */
  tcgetattr (shell_terminal, &j->tmodes);
  tcsetattr (shell_terminal, TCSADRAIN, &shell_tmodes);
  //printf("La commande est au premier plan.");
}

/* Put a job in the background.  If the cont argument is true, send the process group a SIGCONT signal to wake it up.  */
void put_job_in_background (job *j, int cont){
  /* Send the job a continue signal, if necessary.  */
  printf("Le job a été mis en background.\n");
  if (cont)
    if (kill (-j->pgid, SIGCONT) < 0)
      perror ("kill (SIGCONT)");
}

/*Call in launch_job function */
void launch_process (process *p, pid_t pgid, int infile, int outfile, int errfile, int foreground){
  pid_t pid;

  if (shell_is_interactive)
    {
      /* Put the process into the process group and give the process group the terminal, if appropriate.
         This has to be done both by the shell and in the individual child processes because of potential race conditions.  */
      pid = getpid ();
      if (pgid == 0) pgid = pid;
      setpgid (pid, pgid);
      if (foreground)
        tcsetpgrp (shell_terminal, pgid);

      /* Set the handling for job control signals back to the default.  */
      signal (SIGINT, SIG_DFL);
      signal (SIGQUIT, SIG_DFL);
      signal (SIGTSTP, SIG_DFL);
      signal (SIGTTIN, SIG_DFL);
      signal (SIGTTOU, SIG_DFL);
      signal (SIGCHLD, SIG_DFL);
    }

  /* Set the standard input/output channels of the new process.  */
  if (infile != STDIN_FILENO)
    {
      dup2 (infile, STDIN_FILENO);
      close (infile);
    }
  if (outfile != STDOUT_FILENO)
    {
      dup2 (outfile, STDOUT_FILENO);
      close (outfile);
    }
  if (errfile != STDERR_FILENO)
    {
      dup2 (errfile, STDERR_FILENO);
      close (errfile);
    }

  /* Exec the new process.  Make sure we exit.  */
  execvp (p->argv[0], p->argv);
  perror ("execvp");
  exit (1);
}

/* Launch the job. */
void launch_job (job *j, int foreground){
  process *p;
  pid_t pid;
  int mypipe[2], infile, outfile;

  infile = j->stdin;
  for (p = j->first_process; p; p = p->next)
    {
      /* Set up pipes, if necessary.  */
      if (p->next)
        {
          if (pipe (mypipe) < 0)
            {
              perror ("pipe");
              exit (1);
            }
          outfile = mypipe[1];
        }
      else
        outfile = j->stdout;

      /* Fork the child processes.  */
      pid = fork ();
      if (pid == 0)
        /* This is the child process.  */
        launch_process (p, j->pgid, infile,
                        outfile, j->stderr, foreground);
      else if (pid < 0)
        {
          /* The fork failed.  */
          perror ("fork");
          exit (1);
        }
      else
        {
          /* This is the parent process.  */
          p->pid = pid;
          if (shell_is_interactive)
            {
              if (!j->pgid)
                j->pgid = pid;
              setpgid (pid, j->pgid);
            }
        }

      /* Clean up after pipes.  */
      if (infile != j->stdin)
        close (infile);
      if (outfile != j->stdout)
        close (outfile);
      infile = mypipe[0];
    }

  format_job_info (j, "launched");

  if (!shell_is_interactive)
    wait_for_job (j);
  else if (foreground)
    put_job_in_foreground (j, 0);
  else
    put_job_in_background (j, 0);
}


/* We process the command, cut into tokens by "\n" and " ". */
void parse_cmd(char *command, char** argv)
{
  const char *separators = "\n ";
  int i = 0;
  argv[i] = strtok(command, separators);
  while (argv[i] != NULL) {
    i++;
    argv[i] = strtok(NULL, separators);
  }
}

/* We process the command, cut into tokens by "<" or ">". */
void parse_cmd_chevron(char *command, char** argv)
{
  const char *separators = "\n <>";
  int i = 0;
  argv[i] = strtok(command, separators);
  while (argv[i] != NULL) {
    i++;
    argv[i] = strtok(NULL, separators);
  }
}

/*Analyse if argv contain '<' */
bool possede_chevron_gauche(char **argv){
 int i = 0;
 while (argv[i] != NULL) {
 if(strcmp(argv[i], "<") == 0){
 return true;
 } 
 i++;
 }
 return false;
}

/*Analyse if argv contain '>' */
bool possede_chevron_droit(char **argv){
 int i = 0;
 while (argv[i] != NULL) {
 if(strcmp(argv[i], ">") == 0){
 return true;
 } 
 i++;
 }
 return false;
}

/* Manage int/out with '<' or '>' in the command. */
void entree_sortie(job *first_job, char **argv2, char **argv_job){

    first_job->stderr = STDERR_FILENO;
    if(possede_chevron_gauche(argv2) && possede_chevron_droit(argv2)){
      int fd = open(argv_job[1], O_RDONLY);
      int in = dup(fd);
      int fdo = open(argv_job[1], O_WRONLY | O_CREAT, 0644);
      int out = dup(fdo);
      first_job->stdin = in;
      first_job->stdout = out;
    }
    else if(possede_chevron_gauche(argv2)){
      int fd = open(argv_job[1], O_RDONLY);
      int in = dup(fd);
      first_job->stdin = in;
      first_job->stdout = STDOUT_FILENO;
    }
    else if(possede_chevron_droit(argv2)){
      int fdo = open(argv_job[1], O_WRONLY | O_CREAT, 0777);
      int out = dup(fdo);
      first_job->stdout = out;
      first_job->stdin = STDIN_FILENO;
    }else{
      first_job->stdin = STDIN_FILENO;
      first_job->stdout = STDOUT_FILENO;
    } 
}

/*Determine if a command has to be launch in background or in foreground*/
int is_foreground(char *commande)
{
  //printf("size = %lu bytes \n", strlen(commande)); //taille = strlen(command) -1
  if(commande[strlen(commande)-2]=='&'){
	  commande[strlen(commande)-2] = '\0';
    //en background
		return 0;
	}else{
    //en foreground
    return 1;
  }
}

//------------------------------ Our cd, cp and help function ------------------------

/* Our cd function.  */
int commande_cd(const char* rep)
{
 if (chdir(rep) != 0){
  perror("Ce n'est pas un dossier.\n");
 }
 return 0;
}

/* Our cp function.  */
int copie_fichier(const char* fichierACopier, const char* fichierCopie)
{
 // Déclaration variable 
 int fichier = -1;
 char* buffer = (char*)malloc(4096*sizeof(char));
 struct stat buf; 

 // Récupération état du fichier
 stat(fichierACopier, &buf);

 // Ouverture fichier
 fichier = open(fichierACopier, O_RDONLY);
 int fichier_copie = open(fichierCopie, O_WRONLY | O_CREAT);

 // Si le fichier et bien ouvert 
 if (fichier != -1)
 {
 // On peut lire et écrire dans le fichier
 int size_buf = read(fichier, buffer, 4096);
 // read renvoie 0 si il n'y a plus de données dans le buffer
 while(size_buf > 0){
 write(fichier_copie, buffer, size_buf);
 size_buf = read(fichier, buffer, 4096);
 }

 // Libération de la mémoire 
 free(buffer);

 // Fermeture fichiers
 close(fichier);
 close(fichier_copie);
 }
 else
 {
 // On affiche un message d'erreur si on veut
 printf("Impossible d'ouvrir le fichier à copier.");
 return -1;
 }
 chmod(fichierCopie, buf.st_mode);
 return 0;
}


int copie_repertoire(const char* repertoireACopier, const char* repertoireCopie){
 // Déclaration des structures
 DIR *dp_rep;
 struct dirent *dirp;
 struct stat st;

 //Création du répertoire à copier s'il n'existe pas
 if(stat(repertoireACopier, &st)){
 mkdir(repertoireACopier,0777);
 }

 if(stat(repertoireCopie, &st)){
 mkdir(repertoireCopie,0777);
 }

 // Récupération du flux de pointeur d'un répertoire
 dp_rep = opendir(repertoireACopier);

 // Le readdir renvoie NULL quand il arrive à la fin du répertoire
 while ((dirp = readdir(dp_rep)) != NULL){
 
 // Skip les lignes de dp avec . et ..
 if (strcmp (dirp->d_name, "." ) == 0)
 continue;
 if (strcmp (dirp->d_name, ".." ) == 0) 
 continue;

 // Création char des chemins avec malloc défini avec les tailles adaptés
 char *chemin_initial = (char*) malloc(strlen(repertoireACopier) + strlen("/") + strlen(dirp->d_name));
 char *chemin_copie = (char*) malloc(strlen(repertoireCopie) + strlen("/") + strlen(dirp->d_name));

 // Copie répertoire à copier puis ajout éléments pour créer le chemin
 strcpy(chemin_initial,repertoireACopier); 
 strcat(chemin_initial,"/"); 
 strcat(chemin_initial, dirp->d_name); 
 
 // Copie répertoire de copie puis ajout éléments pour créer le chemin
 strcpy(chemin_copie,repertoireCopie); 
 strcat(chemin_copie,"/"); 
 strcat(chemin_copie, dirp->d_name); 
 
 // Récupération état du fichier
 stat(chemin_initial, &st);

 // Vérification si c'est un répertoire
 if (S_ISREG(st.st_mode) == 0) {
 // Création du répertoire enfant dans le répertoire de copie
 mkdir(chemin_copie,0777);
 // Copie les éléments de ce dossier (récursif)
 copie_repertoire(chemin_initial, chemin_copie);

 // Affichage copie pour un répertoire
 printf("Copie effectué : ");
 printf("%s", dirp->d_name);
 printf(" (répertoire)\n");
 }
 // Sinon c'est un dossier
 else{
 FILE* fichier = NULL;
 // Création du fichier dans le répertoire copie (fopen permet de créer fichier s'il n'existe pas)
 fichier = fopen(chemin_copie, "wt");
 // Copie du contenu et permission du fichier à copier
 copie_fichier(chemin_initial,chemin_copie);

 // Affichage copie pour un fichier
 printf("Copie effectué : ");
 printf("%s", dirp->d_name);
 printf(" (fichier)\n");
 }
 // Libération de la mémoire 
 free(chemin_copie);
 free(chemin_initial);
 }
 // Fermeture répertoire
 closedir(dp_rep);
 return 0;
}

/* Our help function. */
int help(){
 printf("\nShell réalisé en juin 2022 par CHEVALLIER Mathis et MARTIN Hugues en langage C.\n");
 printf("Voici les différentes commandes de ce shell avec leur explication : \n\n");
 printf(" cd : Changement de répertoire courant => exemple : cd nomRepertoire ; cd .. (revenir en arrière).\n");
 printf(" cp : Copie d'un fichier => exemple : cp fichierACopier fichierCopie ; cp fichierACopier repertoireCopie/fichierCopie.\n");
 printf(" cp -R : Copie d'un répertoire => exemple : cp -R repertoireACopier repertoireCopie (répertoireCopie peut ne pas exister).\n");
 printf(" help : Explication pour l'utilisation du shell. Vous venez de saisir cette commande pour lire ces explications.\n");
 printf(" quit : Quitte le shell \n");
 printf("Les autres commandes de base comme ls, cat, etc. sont aussi présentes sous forme de processus donc utilisable normalement.\n");
 return 0;
}


int main(int argc, char **argv, char **envp)
{
 init_shell();
 
 char commande[MAX_LENGTH]; //imput
 char commande_v2[MAX_LENGTH]; //copy of commande, the imput

 char *argv2[sizeof(char)*64]; //will stock tokens, have to parse the imput 
 char *argv_job[sizeof(char)*64]; //same as argv2 but for in/out

 char path[MAX_LENGTH];

  printf("-------------------------------------\n");
  printf("\nCHEVALLIER Mathis & MARTIN Hugues\n");
  printf("Projet shell - Juin 2022 - Polytech Paris Saclay \n");
  printf("\n-------------------------------------\n");

 while(1) {
  printf("\n\n");

  /*Print the actual directory path */
  getcwd(path, sizeof(path));
  printf("<");
  printf("%s",path);
  printf("> ");

  /*Wait for user's command */
  fgets(commande, MAX_LENGTH, stdin);

  /*Checks if the argument should be executed in the background */
  int foreground = is_foreground(commande);

  strcpy(commande_v2, commande);

  parse_cmd(commande, argv2);
  parse_cmd_chevron(commande_v2, argv_job);


  /*Choose to execute the right function */
  if(strcmp(argv2[0], "quit") == 0){
    // Execute to quit the shell
    return 0;
  }else if(strcmp(argv2[0], "cd") == 0){
    // We execute the cd command : change of current directory
    commande_cd(argv2[1]);
  }else if(strcmp(argv2[0], "cp") == 0){
    // Execute the command cp : copy a file or directory
    if(strcmp(argv2[1], "-R") == 0){
      // Execute the command cp -R : copy a directory
      copie_repertoire(argv2[2], argv2[3]);
    }
    else{
      // Execute the command cp : copy a file
      copie_fichier(argv2[1], argv2[2]);
    }
  }
  else if(strcmp(argv2[0], "help") == 0){
    // Execute the commande help : description of available command for this shell
    help();
  }
  
  // Creation and launch of processes and jobs
  else{
    first_job = init_job(commande, argv_job);

    entree_sortie(first_job, argv2, argv_job);
    
    
    if (first_job != NULL) {
      //printf("Job créé non null\n");
      launch_job(first_job, foreground);
      free_job(first_job);
    }else{
      perror("Ne cree pas le job, passe a cote...\n");
      exit(1);
    }
  }
  //printf("Ce texte doit s'afficher\n");
 }
 return 0;
}