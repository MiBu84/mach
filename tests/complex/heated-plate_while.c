#include <math.h>
#include <mpi.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

/* ************************************************************************ */
/* helper function: freeMatrix: frees memory of the matrix                  */
/* ************************************************************************ */
static void freeMatrix(double **Matrix) {
  free(Matrix[0]);
  free(Matrix);
}

/* ************************************************************************ */
/* helper function: allocateMatrix: allocates memory for a matrix           */
/* ************************************************************************ */
static double **allocateMatrix(int x, int y) {
  int i;

  double *data_layer = (double *)malloc(x * y * sizeof(double));
  double **resultmatrix = (double **)malloc(x * sizeof(double *));

  if (data_layer == NULL || resultmatrix == NULL) {
    printf("ERROR ALLOCATING MEMORY\n");
    return NULL;
  }

  for (i = 0; i < x; i++) {
    resultmatrix[i] = data_layer + i * y;
  }

  return resultmatrix;
}

int main(int argc, char *argv[])

//
//  Purpose:
//
//    MAIN is the main program for HEATED_PLATE.
//
//  Discussion:
//
//    This code solves the steady state heat equation on a rectangular region.
//
//    The sequential version of this program needs approximately
//    18/epsilon iterations to complete.
//
//
//    The physical region, and the boundary conditions, are suggested
//    by this diagram;
//
//                   W = 0
//             +------------------+
//             |                  |
//    W = 100  |                  | W = 100
//             |                  |
//             +------------------+
//                   W = 100
//
//    The region is covered with a grid of M by N nodes, and an M by N
//    array W is used to record the temperature.  The correspondence between
//    array indices and locations in the region is suggested by giving the
//    indices of the four corners:
//
//                  I = 0
//          [0][0]-------------[0][N-1]
//             |                  |
//      J = 0  |                  |  J = N-1
//             |                  |
//        [M-1][0]-----------[M-1][N-1]
//                  I = M-1
//
//    The steady state solution to the discrete heat equation satisfies the
//    following condition at an interior grid point:
//
//      W[Central] = (1/4) * ( W[North] + W[South] + W[East] + W[West] )
//
//    where "Central" is the index of the grid point, "North" is the index
//    of its immediate neighbor to the "north", and so on.
//
//    Given an approximate solution of the steady state heat equation, a
//    "better" solution is given by replacing each interior point by the
//    average of its 4 neighbors - in other words, by using the condition
//    as an ASSIGNMENT statement:
//
//      W[Central]  <=  (1/4) * ( W[North] + W[South] + W[East] + W[West] )
//
//    If this process is repeated often enough, the difference between
//    successive estimates of the solution will go to zero.
//
//    This program carries out such an iteration, using a tolerance specified by
//    the user.
//
//  Licensing:
//
//    This code is distributed under the GNU LGPL license.
//
//  Modified:
//
//    24 October 2019
//
//  Author:
//
//    Original C version by Michael Quinn.
//    adapted by Tim Jammer.
//
//  Reference:
//
//    Michael Quinn,
//    Parallel Programming in C with MPI and OpenMP,
//    McGraw-Hill, 2004,
//    ISBN13: 978-0071232654,
//    LC: QA76.73.C15.Q55.
//
//  Local parameters:
//
//    Local, double DIFF, the norm of the change in the solution from one
//    iteration to the next.
//
//    Local, double MEAN, the average of the boundary values, used to initialize
//    the values of the solution in the interior.
//
//    Local, double U[M][N], the solution at the previous iteration.
//
//    Local, double W[M][N], the solution computed at the latest iteration.
//
{
#define M 500
#define N 500

  // message tag
#define tag 42

#define NUM_ITER 16384

  int rank, numtasks;
  // Initialisiere Alle Prozesse
  MPI_Init(&argc, &argv);

  // Welchen rang habe ich?
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  // wie viele Tasks gibt es?
  MPI_Comm_size(MPI_COMM_WORLD, &numtasks);

  double diff = 1;
  double epsilon = 0.001;
  int i;
  int iterations;
  int iterations_print;
  int j;
  double mean;

  if (rank == 0) {
    printf("A program to solve for the steady state temperature distribution\n"
           "over a rectangular plate.\n\n"
           "Spatial grid of %d by %d points.\n"
           "The iteration will be repeated until the change is <= %f\n",
           M, N, epsilon);
  }

  int from, to, size; // lokaler bereich der Matrix
  int pre, nxt;       // kommunikationspartner
  int rest;

  // Der Rest wird hier Zeilenweise auf die Ersten Prozesse aufgeteilt
  // Das Bedeutet bei rest X, dass die Ersten X Prozesse eine Zeile mehr
  // bekommen beispiel: 3 Prozesse 8 Zeilen: Prozess0: 3 Zeilen Prozess1: 3
  // Zeilen Prozess2: 2 Zeilen

  size = (M - 1) / numtasks;
  rest = (M - 1) % numtasks;
  //-1, da die Letzte Zeile zu keinem Prozess gehört (die 0te Zeile gehört auch
  //keinem, wird aber nicht mitgezählt)
  from = rank * size + rest + 1;
  //+1 bei, da die erste Zeile zu keinem Prozess gehört

  if (rest > rank) {
    size++;
    from = rank * size + 1;
  }
  to = from + size - 1;

  if (size <= 1) {
    printf("Zu Wenige Zeilen, um sie sinvoll aufzuteilen, Bitte Starten sie "
           "das Programm mit weniger Prozessen");
    return -1;
  }

  // Nachfolger und vorgänger initialisieren
  if (numtasks == 1) // Dann Keine Kommunikation
  {
    pre = -1;
    nxt = -1;
  } else {
    pre = rank - 1; // Funktioniert auch für den Sonderfall Rank=0
    nxt = rank + 1;
    if (nxt >= numtasks) {
      nxt = -1; // Sonderfall: Letzter Task
    }
  }

  double **w = allocateMatrix(size + 2, N);
  double **u = allocateMatrix(size + 2, N);

  //
  //  Set the boundary values, which don't change.
  //
  mean = 0.0;
  for (i = 1; i < size; i++) {
    w[i][0] = 100.0;
    w[i][N - 1] = 100.0;
  }

  // nur der letzte rank hat diesen teil der Matrix
  if (rank == numtasks - 1) {
    for (j = 0; j < N; j++) {
      w[size][j] = 100.0;
    }
  }

  // nur auf rank0
  if (rank == 0) {
    for (j = 0; j < N; j++) {
      w[0][j] = 0.0;
    }
  }
  //
  //  Average the boundary values, to come up with a reasonable
  //  initial value for the interior.
  //
  // die Schleife durch den vorher berechneten wert ersetzt
  // dann können wir auf einen broadcast verzichten
  mean = N * 100 + N * 0 + M * 100 + M * 100 - 4 * 100;
  // -6 für die ecken, die dopplet getzählt wurde und am unteren rand 0 sind

  mean = mean / (double)(2 * M + 2 * N - 4);
  if (rank == 0) {
    printf("\n MEAN %f\n", mean);
  }
  //
  //  Initialize the interior solution to the mean value.
  //
  // nur den eigenen teil initialisieren
  for (i = 1; i < size; i++) {
    for (j = 1; j < N - 1; j++) {
      w[i][j] = mean;
    }
  }

  iterations = 0;
  iterations_print = 1;
  if (rank == 0) {
    printf("\n"
           " Iteration  Change\n"
           "\n");
  }
  // barirer for time measurment
  MPI_Barrier(MPI_COMM_WORLD);
  time_t start = time(0);

  // Speicher, damit der Status der Verschiedenen Kommunikationsvorgänge am ende
  // abgefragt werden kann
  MPI_Request reqSendPre;
  MPI_Request reqSendNxt;
  MPI_Request reqRcvPre;
  MPI_Request reqRcvNxt;
  MPI_Status status;

  while (diff > epsilon) {

    //
    //  Save the old solution in U.
    // nur auf der eigenen Matrix arbeiten

    for (i = 1; i < size; i++) {
      for (j = 0; j < N; j++) {
        u[i][j] = w[i][j];
      }
    }
    // Berechne Zeile1
    i = 1;
    for (j = 1; j < N - 1; j++) {
      w[i][j] = (u[i - 1][j] + u[i + 1][j] + u[i][j - 1] + u[i][j + 1]) / 4.0;
    }

    // Sende Das Ergebnis und empfange die Zeile für die Nächste iteration
    // Das geht an dieseer Stelle non blocked, da die Buffer verschieden sind
    // und im Weiteren Verlauf der Iteration nihct mehr benutzt werden müssen
    if (pre != -1) {
      MPI_Isend(w[i], N, MPI_DOUBLE, pre, tag, MPI_COMM_WORLD, &reqSendPre);
      // Empfangene Nachicht= 0te Zeile der Neuen Matrix
      MPI_Irecv(w[i - 1], N, MPI_DOUBLE, pre, tag, MPI_COMM_WORLD, &reqRcvPre);
    }

    // Berechne Zeile N-1

    i = size - 1;
    for (j = 1; j < N - 1; j++) {
      w[i][j] = (u[i - 1][j] + u[i + 1][j] + u[i][j - 1] + u[i][j + 1]) / 4.0;
    }

    if (nxt != -1) {
      MPI_Isend(w[i], N, MPI_DOUBLE, nxt, tag, MPI_COMM_WORLD, &reqSendNxt);

      MPI_Irecv(w[i + 1], N, MPI_DOUBLE, nxt, tag, MPI_COMM_WORLD, &reqRcvNxt);
    }

    /* alle anderen Zeilen */

    for (i = 2; i < size - 1; i++) {
      for (j = 1; j < N - 1; j++) {
        w[i][j] = (u[i - 1][j] + u[i + 1][j] + u[i][j - 1] + u[i][j + 1]) / 4.0;
      }
    }

    diff = 0.0;

    for (i = 1; i < size; i++) {
      for (j = 1; j < N - 1; j++) {
        if (diff < fabs(w[i][j] - u[i][j])) {
          diff = fabs(w[i][j] - u[i][j]);
        }
      }
    }

    MPI_Allreduce(MPI_IN_PLACE, &diff, 1, MPI_DOUBLE, MPI_MAX, MPI_COMM_WORLD);

    // Warte auf abschluss der Kommunikation
    if (pre != -1) {
      MPI_Wait(&reqSendPre, &status);
      MPI_Wait(&reqRcvPre, &status);
    }
    if (nxt != -1) {
      MPI_Wait(&reqSendNxt, &status);
      MPI_Wait(&reqRcvNxt, &status);
    }

    iterations++;
    if (iterations == iterations_print && rank == 0) {
      iterations_print = 2 * iterations_print;

      printf("  %8d  %f\n", iterations, diff);
    }
  } // end for iterations

  // barirer for time measurment
  MPI_Barrier(MPI_COMM_WORLD);
  time_t end = time(0);

  double elapsed = difftime(end, start);

  if (rank == 0) {
    printf("\n"
           "  %8d  %f\n"
           "\n"
           "  Error tolerance achieved: %f\n"
           "  time elapsed = %f\n"
           "  Normal end of execution.\n",
           iterations, diff, diff, elapsed);
  }

  //
  //  Terminate.
  //
  freeMatrix(u);
  freeMatrix(w);

  MPI_Finalize();

  return 0; // barirer for time measurment

#undef M
#undef N
}
